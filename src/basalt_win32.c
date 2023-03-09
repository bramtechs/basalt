#include <windows.h>
#include "basalt.h"

#define STBI_NO_STDIO
#define STB_IMAGE_IMPLEMENTATION
#include "external/stb_image.h"
#include "assets_core.h"

typedef struct {
    // NOTE(casey): Pixels are alwasy 32-bits wide, Memory Order BB GG RR XX
    BITMAPINFO Info;
    uint* Memory;
    int Width;
    int Height;
    int Pitch;
} OffscreenBuffer;

typedef struct {
    int Width;
    int Height;
} Dimension;

// TODO(casey): This is a global for now.
static bool GlobalRunning;
static OffscreenBuffer GlobalBackbuffer;

// assets
Texture LoadTexture(uchar* pixels){
    uint len = 0;

    volatile uint i=0x01234567;

    uchar a1 = pixels[0];
    uchar a2 = pixels[1];
    uchar a3 = pixels[2];
    uchar a4 = pixels[3];

    if (IsLittleEndian()) {
        len = ((uint)pixels[0] << 24) |
              ((uint)pixels[1] << 16) |
              ((uint)pixels[2] << 8)  |
              ((uint)pixels[3] << 0);
    } else {
        len = *(uint*)pixels;
    }

    const stbi_uc* data = (const stbi_uc*) &pixels[4];

    Texture texture;
    int channels = 0;

    uint* imgData = (uint*) stbi_load_from_memory(data, len, &texture.width, &texture.height, &channels, 4);
    if (imgData == NULL) {
        exit(1);
    }

    // TODO: deal with 3 channels
    // TODO: check if windows can draw RGBA so this doesn't need to be converted
    int pixelCount = texture.width * texture.height;

    // rearrange each pixel to be BGRA (for windows bitmaps)
    uint* pixel = imgData;
    for (int i = 0; i < pixelCount; i++){

        // Extract the red, green, blue, and alpha channels
        // TODO: find out why this is weird
        uchar green = (*pixel >> 0)   & 0xFF;
        uchar red =   (*pixel >> 8)   & 0xFF;
        uchar alpha = (*pixel >> 16)  & 0xFF;
        uchar blue =  (*pixel >> 24)  & 0xFF;
       
        // Create a new BGRA color value by swapping the red and blue channels
        uint bgraColor = (blue << 24) | (green << 16) | (red << 8) | alpha;

        // WARN: swap is done in place, so don't do stb operations after this!
        *pixel = bgraColor;
        pixel++;
    }

    texture.pixels = imgData;
    assert(channels == 4);

    return texture;
}

Dimension Win32GetWindowDimension(HWND Window)
{
    Dimension Result;
    
    RECT ClientRect;
    GetClientRect(Window, &ClientRect);
    Result.Width = ClientRect.right - ClientRect.left;
    Result.Height = ClientRect.bottom - ClientRect.top;

    return(Result);
}

static void RenderWeirdGradient(OffscreenBuffer Buffer, int BlueOffset, int GreenOffset)
{
    // TODO(casey): Let's see what the optimizer does

    uchar *Row = (uchar *)Buffer.Memory;    
    for(int Y = 0;
        Y < Buffer.Height;
        ++Y)
    {
        uint *Pixel = (uint *)Row;
        for(int X = 0;
            X < Buffer.Width;
            ++X)
        {
            uchar Blue = (X + BlueOffset);
            uchar Green = (Y + GreenOffset);

            *Pixel++ = ((Green << 16) | Blue);
        }
        
        Row += Buffer.Pitch;
    }
}

static void RenderSprite(OffscreenBuffer buffer, Texture texture, int posX, int posY){
    uint width = texture.width;
    uint height = texture.height;

    uint *pixels = (uint*) buffer.Memory;

    int j = 0;
    for (int y = 0; y < height; y++){
        for (int x = 0; x < width; x++){
            int xx = posX + x;
            int yy = posY + y;
            int i = yy * buffer.Width + xx;
            pixels[i] = texture.pixels[j++];
        }
    }

}

static void Win32ResizeDIBSection(OffscreenBuffer *Buffer, int Width, int Height)
{
    // TODO(casey): Bulletproof this.
    // Maybe don't free first, free after, then free first if that fails.

    if(Buffer->Memory)
    {
        VirtualFree(Buffer->Memory, 0, MEM_RELEASE);
    }

    Buffer->Width = Width;
    Buffer->Height = Height;

    int BytesPerPixel = 4;

    // NOTE(casey): When the biHeight field is negative, this is the clue to
    // Windows to treat this bitmap as top-down, not bottom-up, meaning that
    // the first three bytes of the image are the color for the top left pixel
    // in the bitmap, not the bottom left!
    Buffer->Info.bmiHeader.biSize = sizeof(Buffer->Info.bmiHeader);
    Buffer->Info.bmiHeader.biWidth = Buffer->Width;
    Buffer->Info.bmiHeader.biHeight = -Buffer->Height;
    Buffer->Info.bmiHeader.biPlanes = 1;
    Buffer->Info.bmiHeader.biBitCount = 32;
    Buffer->Info.bmiHeader.biCompression = BI_RGB;

    // NOTE(casey): Thank you to Chris Hecker of Spy Party fame
    // for clarifying the deal with StretchDIBits and BitBlt!
    // No more DC for us.
    int BitmapMemorySize = (Buffer->Width*Buffer->Height)*BytesPerPixel;
    Buffer->Memory = (uint *) VirtualAlloc(0, BitmapMemorySize, MEM_COMMIT, PAGE_READWRITE);
    Buffer->Pitch = Width*BytesPerPixel;

    // TODO(casey): Probably clear this to black
}

static void Win32DisplayBufferInWindow(HDC DeviceContext,
                                       int WindowWidth, int WindowHeight,
                                       OffscreenBuffer Buffer)
{
    // TODO(casey): Aspect ratio correction
    // TODO(casey): Play with stretch modes
    StretchDIBits(DeviceContext,
                  /*
                  X, Y, Width, Height,
                  X, Y, Width, Height,
                  */
                  0, 0, WindowWidth, WindowHeight,
                  0, 0, Buffer.Width, Buffer.Height,
                  Buffer.Memory,
                  &Buffer.Info,
                  DIB_RGB_COLORS, SRCCOPY);
}

LRESULT CALLBACK Win32MainWindowCallback(HWND Window,
                                         UINT Message,
                                         WPARAM WParam,
                                         LPARAM LParam)
{       
    LRESULT Result = 0;

    switch(Message)
    {
        case WM_CLOSE:
        {
            // TODO(casey): Handle this with a message to the user?
            GlobalRunning = false;
        } break;

        case WM_ACTIVATEAPP:
        {
            OutputDebugStringA("WM_ACTIVATEAPP\n");
        } break;

        case WM_DESTROY:
        {
            // TODO(casey): Handle this as an error - recreate window?
            GlobalRunning = false;
        } break;
        
        case WM_PAINT:
        {
            PAINTSTRUCT Paint;
            HDC DeviceContext = BeginPaint(Window, &Paint);
            Dimension Dimension = Win32GetWindowDimension(Window);
            Win32DisplayBufferInWindow(DeviceContext, Dimension.Width, Dimension.Height,
                                       GlobalBackbuffer);
            EndPaint(Window, &Paint);
        } break;

        default:
        {
//            OutputDebugStringA("default\n");
            Result = DefWindowProc(Window, Message, WParam, LParam);
        } break;
    }
    
    return(Result);
}

#ifndef RELEASE
void UnitTest(){
    CheckTypes();

    // spr block size example
    uchar result[4];
    uint value = 1770;
    if (IsLittleEndian()) {
        result[3] = (uchar)(value >> 24);
        result[2] = (uchar)(value >> 16);
        result[1] = (uchar)(value >> 8);
        result[0] = (uchar)(value >> 0);
    }
    else {
        result[0] = (uchar)(value >> 24);
        result[1] = (uchar)(value >> 16);
        result[2] = (uchar)(value >> 8);
        result[3] = (uchar)(value >> 0);
    }
    uint val = *((uint*)result);
    assert(val == 1770);
}
#endif

int CALLBACK
WinMain(HINSTANCE Instance,
        HINSTANCE PrevInstance,
        LPSTR CommandLine,
        int ShowCode)
{
    WNDCLASS WindowClass = { 0 };

    Win32ResizeDIBSection(&GlobalBackbuffer, 1280, 720);
    
    WindowClass.style = CS_HREDRAW|CS_VREDRAW|CS_OWNDC;
    WindowClass.lpfnWndProc = Win32MainWindowCallback;
    WindowClass.hInstance = Instance;
//    WindowClass.hIcon;
    WindowClass.lpszClassName = "HandmadeHeroWindowClass";

    UnitTest();

    if(RegisterClassA(&WindowClass))
    {
        HWND Window =
            CreateWindowExA(
                0,
                WindowClass.lpszClassName,
                "Handmade Hero",
                WS_OVERLAPPEDWINDOW|WS_VISIBLE,
                CW_USEDEFAULT,
                CW_USEDEFAULT,
                CW_USEDEFAULT,
                CW_USEDEFAULT,
                0,
                0,
                Instance,
                0);
        if(Window)
        {
            // NOTE(casey): Since we specified CS_OWNDC, we can just
            // get one device context and use it forever because we
            // are not sharing it with anyone.
            HDC DeviceContext = GetDC(Window);

            int XOffset = 0;
            int YOffset = 0;

            GlobalRunning = true;

            Texture colorSprite = LoadTexture(SPR_RGBA);
            Texture foxSprite = LoadTexture(SPR_PLAYER_FOX);
            Texture blockSprite = LoadTexture(SPR_BLOCK);
            Texture smallBlockSprite = LoadTexture(TILE_BLOCK_SMALL);
            
            while(GlobalRunning)
            {
                MSG Message;

                while(PeekMessage(&Message, 0, 0, 0, PM_REMOVE))
                {
                    if(Message.message == WM_QUIT)
                    {
                        GlobalRunning = false;
                    }
                    
                    TranslateMessage(&Message);
                    DispatchMessageA(&Message);
                }

                RenderWeirdGradient(GlobalBackbuffer, XOffset, YOffset);

                RenderSprite(GlobalBackbuffer, colorSprite, 300, 250);
                RenderSprite(GlobalBackbuffer, foxSprite, 10, 10);
                RenderSprite(GlobalBackbuffer, blockSprite, 10, foxSprite.height + 10);
                RenderSprite(GlobalBackbuffer, smallBlockSprite, blockSprite.width + 10, foxSprite.height + 10);

                Dimension Dimension = Win32GetWindowDimension(Window);
                Win32DisplayBufferInWindow(DeviceContext, Dimension.Width, Dimension.Height,
                                           GlobalBackbuffer);
                
                ++XOffset;
                YOffset += 2;
            }
        }
        else
        {
            // TODO(casey): Logging
        }
    }
    else
    {
        // TODO(casey): Logging
    }
    
    return(0);
}
