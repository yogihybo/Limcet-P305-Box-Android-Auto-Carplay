#ifndef MINIVGUI_H_INCLUDED
#define MINIVGUI_H_INCLUDED
#define mv_RGB_FORMAT_555	555
#define mv_RGB_FORMAT_565	565
#define mv_RGB_FORMAT_888	888
#define mv_RGB_FORMAT_666	666
#define BSP_DISP_RGB_565			5
#define BSP_DISP_RGB_888			7
//change this line if you are using other rgb format
#define mv_RGB_FORMAT mv_RGB_FORMAT_888
#if ( mv_RGB_FORMAT == mv_RGB_FORMAT_565)
	typedef unsigned short	mv_color;
#elif ( mv_RGB_FORMAT == mv_RGB_FORMAT_555)
	typedef unsigned short	mv_color;
#elif ( mv_RGB_FORMAT == mv_RGB_FORMAT_888)
	typedef unsigned int mv_color;
#elif ( mv_RGB_FORMAT == mv_RGB_FORMAT_666)
	typedef unsigned int mv_color;
#else
	#error "unknown rgb format."
#endif
typedef struct
{
	int	width;
	int	height;
	int	lineBytes;
	int	bpp;
	char *startAddr;
}mv_surface;
typedef struct
{
	int	left;
	int	top;
	int	right;
	int	bottom;
}mv_Rect;
/**
 * init minivgui primary screen. this is the first function need be called
 */
void mv_initPrimary(const mv_surface * s);
// bmpData is the start address of a Win32 bmp file(support 32/24/16/8bit format)
// usually it is loaded from nand flash
// img->startAddr must be a valid memory address and can hold the image data
// return 0 if ok.
int mv_loadBmp(const void* bmpData, mv_surface* img);
int mv_loadBmp16(const void* bmpData, mv_surface* img);
int mv_loadBmp32(const void* bmpData, mv_surface* img);
void mv_putPixel(int x, int y, mv_color color);
void mv_putPixel16(int x, int y, unsigned short color);
void mv_putPixel32(int x, int y, unsigned int color);
void mv_drawLine(int x1, int y1, int x2, int y2, mv_color color);
void mv_drawLine16(int x1, int y1, int x2, int y2, unsigned short color);
void mv_drawLine32(int x1, int y1, int x2, int y2, unsigned int color);
void mv_drawRect(const mv_Rect* rect, mv_color color);
void mv_drawRect16(const mv_Rect* rect, unsigned short color);
void mv_drawRect32(const mv_Rect* rect, unsigned int color);
void mv_fillRect(const mv_Rect* rect, mv_color color);
void mv_fillRect16(const mv_Rect* rect, unsigned short color);
void mv_fillRect32(const mv_Rect* rect, unsigned int color);
void Test_LCD_dis_Str(void);
/**
 *
 * @param x start x
 * @param y start y
 * @param bmp the bitmap object
 * @param bmpRect the draw part rect(if NULL draw whole bmp)
 * @param transColor (set the transparent color, if NULL draw bmp directly)
 */
void mv_drawBitmap(int x, int y, const mv_surface* bmp, const mv_Rect * bmpRect,
					const mv_color * transColor);
void mv_drawBitmap16(int x, int y, const mv_surface* bmp, const mv_Rect * bmpRect,
						const unsigned short * transColor);
void mv_drawBitmap32(int x, int y, const mv_surface* bmp, const mv_Rect * bmpRect,
					const unsigned int * transColor);
/**
 * draw text on the screen (Note: only acsii supported)
 */
int mv_textOut(int x, int y, const char * string, mv_color textColor);
int mv_textOut16(int x, int y, const char * string, unsigned short textColor);
int mv_textOut32(int x, int y, const char * string, unsigned int textColor);
unsigned int mv_RGB2Color(unsigned char r, unsigned char g, unsigned char b);
#endif
