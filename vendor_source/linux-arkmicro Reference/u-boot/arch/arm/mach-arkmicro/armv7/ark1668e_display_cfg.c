#include <common.h>
#include <dm.h>
#include <display.h>
#include "ark1668e_lcd.h"
#include <asm/arch/ark-common.h>
#include "URBprobar.h"
#define  UPDATING_WIDTH      200
#define  UPDATING_HEIGHT     20
#define  PROGRESS_WIDTH     200
#define  PROGRESS_HEIGHT     32
#define  UPDATE_LOGO_WIDTH   PROGRESS_WIDTH
#define  UPDATE_LOGO_HEIGHT  (UPDATING_HEIGHT+PROGRESS_HEIGHT)
//struct SYS_INFO sys_info;
static void update_bar_set(int percent,int id);
void update_bar_init(unsigned int width,unsigned int height,unsigned int bpp);
#if 1
//chinese
#define  UPDATING_POINT_COUNT  397
static unsigned short updating[UPDATING_POINT_COUNT] =
{
	810, 821, 828, 840, 866, 878, 883, 894, 900, 1008, 1009, 1010, 1021, 1024, 1025, 1026,
	1027, 1028, 1040, 1060, 1066, 1076, 1077, 1078, 1079, 1080, 1083, 1086, 1093, 1094, 1096, 1097,
	1098, 1099, 1100, 1206, 1207, 1210, 1220, 1223, 1224, 1225, 1227, 1228, 1240, 1260, 1261, 1263,
	1264, 1265, 1266, 1267, 1268, 1269, 1275, 1276, 1277, 1280, 1281, 1283, 1285, 1293, 1299, 1404,
	1405, 1406, 1407, 1410, 1420, 1422, 1425, 1427, 1436, 1440, 1441, 1442, 1443, 1444, 1445, 1461,
	1466, 1467, 1468, 1477, 1481, 1483, 1484, 1492, 1494, 1498, 1499, 1500, 1501, 1502, 1603, 1606,
	1610, 1619, 1621, 1622, 1624, 1627, 1628, 1629, 1636, 1637, 1638, 1639, 1640, 1641, 1644, 1645,
	1664, 1665, 1666, 1667, 1669, 1677, 1678, 1679, 1680, 1683, 1684, 1685, 1686, 1692, 1694, 1695,
	1696, 1697, 1698, 1702, 1806, 1810, 1812, 1813, 1814, 1818, 1819, 1820, 1821, 1824, 1825, 1826,
	1827, 1828, 1829, 1836, 1840, 1844, 1859, 1860, 1861, 1862, 1863, 1864, 1865, 1866, 1867, 1868,
	1869, 1870, 1874, 1875, 1876, 1877, 1880, 1881, 1882, 1883, 1884, 1885, 1891, 1892, 1894, 1896,
	1897, 2002, 2003, 2004, 2005, 2006, 2007, 2008, 2009, 2010, 2011, 2014, 2020, 2024, 2025, 2028,
	2036, 2040, 2044, 2058, 2059, 2060, 2063, 2068, 2076, 2077, 2078, 2080, 2085, 2090, 2092, 2094,
	2096, 2097, 2098, 2099, 2100, 2101, 2206, 2210, 2219, 2220, 2221, 2222, 2223, 2224, 2226, 2227,
	2228, 2236, 2237, 2238, 2239, 2240, 2241, 2242, 2243, 2244, 2245, 2260, 2264, 2265, 2266, 2267,
	2268, 2276, 2277, 2279, 2280, 2281, 2282, 2283, 2284, 2285, 2292, 2294, 2295, 2296, 2298, 2406,
	2410, 2419, 2420, 2423, 2426, 2427, 2436, 2440, 2460, 2464, 2465, 2466, 2467, 2468, 2475, 2477,
	2480, 2485, 2492, 2494, 2498, 2499, 2500, 2501, 2502, 2606, 2610, 2621, 2622, 2623, 2626, 2627,
	2640, 2660, 2662, 2663, 2664, 2668, 2674, 2675, 2677, 2680, 2681, 2682, 2683, 2684, 2685, 2692,
	2694, 2695, 2696, 2697, 2698, 2805, 2810, 2820, 2821, 2822, 2825, 2826, 2827, 2840, 2851, 2852,
	2860, 2861, 2862, 2864, 2865, 2866, 2867, 2868, 2874, 2877, 2880, 2885, 2892, 2897, 2899, 3004,
	3005, 3010, 3018, 3019, 3021, 3022, 3024, 3025, 3028, 3040, 3051, 3052, 3060, 3061, 3064, 3068,
	3077, 3080, 3083, 3085, 3092, 3096, 3100, 3106, 3107, 3114, 3115, 3122, 3123, 3203, 3204, 3210,
	3221, 3223, 3224, 3228, 3229, 3230, 3240, 3252, 3264, 3266, 3267, 3268, 3277, 3280, 3284, 3285,
	3292, 3295, 3296, 3300, 3301, 3403, 3422, 3423, 3451, 3464, 3494, 3495, 3502,
};
#else
//english
#define  UPDATING_POINT_COUNT  237
static unsigned short updating[UPDATING_POINT_COUNT] =
{
	1021, 1022, 1023, 1044, 1045, 1222, 1223, 1235, 1236, 1422, 1423, 1435, 1436, 1601, 1602, 1603,
	1605, 1606, 1607, 1609, 1610, 1611, 1612, 1613, 1614, 1619, 1620, 1621, 1622, 1623, 1627, 1628,
	1629, 1630, 1633, 1634, 1635, 1636, 1637, 1638, 1642, 1643, 1644, 1645, 1649, 1650, 1652, 1653,
	1654, 1659, 1660, 1661, 1662, 1663, 1802, 1803, 1806, 1807, 1810, 1811, 1814, 1815, 1818, 1819,
	1822, 1823, 1826, 1827, 1830, 1831, 1835, 1836, 1844, 1845, 1850, 1851, 1852, 1854, 1855, 1858,
	1859, 1861, 1862, 2002, 2003, 2006, 2007, 2010, 2011, 2014, 2015, 2018, 2019, 2022, 2023, 2028,
	2029, 2030, 2031, 2035, 2036, 2044, 2045, 2050, 2054, 2055, 2058, 2059, 2061, 2062, 2202, 2203,
	2206, 2207, 2210, 2211, 2214, 2215, 2218, 2219, 2222, 2223, 2227, 2228, 2230, 2231, 2235, 2236,
	2244, 2245, 2250, 2254, 2255, 2259, 2260, 2261, 2402, 2403, 2406, 2407, 2410, 2411, 2414, 2415,
	2418, 2419, 2422, 2423, 2426, 2427, 2430, 2431, 2435, 2436, 2444, 2445, 2450, 2454, 2455, 2458,
	2459, 2466, 2467, 2468, 2474, 2475, 2476, 2482, 2483, 2484, 2603, 2604, 2605, 2606, 2607, 2608,
	2610, 2611, 2612, 2613, 2614, 2619, 2620, 2621, 2622, 2623, 2624, 2627, 2628, 2629, 2630, 2631,
	2632, 2636, 2637, 2638, 2642, 2643, 2644, 2645, 2646, 2647, 2649, 2650, 2651, 2652, 2654, 2655,
	2656, 2658, 2659, 2660, 2661, 2662, 2666, 2667, 2668, 2674, 2675, 2676, 2682, 2683, 2684, 2810,
	2811, 2858, 2859, 2862, 2863, 3009, 3010, 3011, 3012, 3059, 3060, 3061, 3062,
};
#endif
#define UPDATELOGO_DISPLAY_ADDR	       0x4F000000
static int screen_width, screen_height;
static struct udevice *g_dp;
static void updating_init(void)
{
	int i;
	unsigned int *p = (unsigned int *)(UPDATELOGO_DISPLAY_ADDR);
	unsigned int color = 0xffffffff;
	int pos = 0;
	for (i = 0; i < UPDATING_WIDTH*UPDATING_HEIGHT; i++) {
		if (updating[pos] == i) {
			*p = color;
			pos++;
		}
		p++;
	}
}
static void update_progress_init(void)
{
	int i, j;
	unsigned int *p = (unsigned int *)(UPDATELOGO_DISPLAY_ADDR + UPDATING_HEIGHT*PROGRESS_WIDTH*4);
	int height = UPDATE_LOGO_HEIGHT;
	unsigned int color = 0xffffffff;
	for (j = UPDATING_HEIGHT; j < height; j++) {
		for (i = 0; i < PROGRESS_WIDTH; i++) {
			if (j < (UPDATING_HEIGHT + 2) || j > (height -3) || \
				i < 2 || i > (PROGRESS_WIDTH -3)) {
				*p = color;
			}
			p++;
		}
	}
}
static void update_progress_set(int percent)
{
	int i, j, end;
	unsigned int *p = (unsigned int *)(UPDATELOGO_DISPLAY_ADDR + UPDATING_HEIGHT*PROGRESS_WIDTH*4);
	int height = UPDATE_LOGO_HEIGHT;
	unsigned int color = 0xff00ff00;
	if (percent < 0 || percent > 100) {
		printf("update progress percent=%d, error.",percent);
		return;
	}
	end = percent * (PROGRESS_WIDTH-3)/100;
	for (j = UPDATING_HEIGHT; j < height; j++) {
		for (i = 0; i < PROGRESS_WIDTH; i++) {
			if (j > (UPDATING_HEIGHT + 2) && j < (height -3) \
				&& i > 2 && i < end) {
				*p = color;
			}
			p++;
		}
	}
	if (percent == 100) {
		udelay(500000);
		ark_osd_en_layer(OSD2_LAYER, 0);
		if (g_dp)
			display_disable(g_dp);
	} else if (percent >= 90) {
		udelay(200000);
	}
}
#if 1
static void display_updatelogo(void)
{
//	int x, y;
//	unsigned int width = UPDATE_LOGO_WIDTH;
//	unsigned int height = UPDATE_LOGO_HEIGHT;
//	memset((void *)UPDATELOGO_DISPLAY_ADDR, 0 , width * height * 4);
//	updating_init();
//	update_progress_init();
	memset((void *)UPDATELOGO_DISPLAY_ADDR, 0 , screen_width*screen_height*4);
	update_bar_init(screen_width,screen_height,32);
	ark_set_osd_image(OSD2_LAYER, DISP_RGB_888, screen_width, screen_height);
	ark_set_osd_addr(OSD2_LAYER, UPDATELOGO_DISPLAY_ADDR);
//	x = (screen_width - width) / 2;
//	y = (screen_height - height) / 2;
//	ark_disp_set_osd_layer_position(OSD2_LAYER, x, y);
}
#endif
void ark_display_init(void)
{
	struct udevice *dp;
	struct display_timing timing;
	int ret;
	ret = uclass_first_device(UCLASS_DISPLAY, &dp);
	if (ret) {
		printf("%s: Display device error %d\n", __func__, ret);
		return;
	}
	g_dp = dp;
	ret = display_read_timing(dp, &timing);
	if (ret) {
		printf("%s: Display read timing error %d\n", __func__, ret);
		return;
	}
	screen_width = timing.hactive.typ;
	screen_height = timing.vactive.typ;
	display_updatelogo();
	mdelay(10);
	ret = display_enable(dp, 24, NULL);
	if (ret) {
		printf("%s: Display enable error %d\n", __func__, ret);
		return;
	}
}
int do_disconfig (cmd_tbl_t *cmdtp, int flag, int argc, char * const argv[])
{
	unsigned long percent = 0;
	int ret;
	if(argc < 2)
		return cmd_usage(cmdtp);
	ret = strict_strtoul(argv[1], 10, &percent);
	if (ret < 0) {
		printf("error format screen id\n");
	}
	if (percent > 100) {
		printf("Invalide param.\n");
		return cmd_usage(cmdtp);
	}
    if (percent == 0) {
        ark_display_init();
    } else {
        //update_progress_set(percent);
		update_bar_set(percent,0);
    }
	return 0;
}
/***************************************************/
U_BOOT_CMD(
	disconfig,	CONFIG_SYS_MAXARGS,	0,	do_disconfig,
	"config display",
	"disconfig percent\n"
	"percent(0:) \n"
);
static void update_bar_set(int percent,int id)
{
//    struct screen_info *screen = &screens[id];
    int x_start = 240;
    int y_start = 220;
    int bar_height = 20;
    mv_Rect rect;
    unsigned int i;
	x_start = (screen_width - 150) / 2;
	y_start = (screen_height - 20) / 2;
	for(i = 0;i< percent;)
	{
		rect.left = x_start +  i;
		rect.right = rect.left + i;
		rect.top = y_start;
		rect.bottom = rect.top + bar_height;
		mv_fillRect(&rect, mv_RGB2Color(00, 255, 00));
		i=i+4;
	}
}
void update_bar_init(unsigned int width,unsigned int height,unsigned int bpp)
{
	int x_start ;//= 240;
	int y_start ;//= 220;
	int bar_height = 21;
	mv_Rect rect;
	int percent = 100;
    unsigned int i;
	mv_surface s;
	s.lineBytes = width * 4;
	s.width = width;
	s.height = height;
	s.bpp = bpp;
	s.startAddr = (char *)UPDATELOGO_DISPLAY_ADDR;
	mv_initPrimary(&s);
	x_start = (width - 150) / 2;
	y_start = (height - 21) / 2;
	mv_textOut(x_start, y_start-21, "System Update Software.....", mv_RGB2Color(0, 255, 0));
	for(i = 0;i< percent;)
	{
		rect.left = x_start +i;
		rect.right = rect.left+i;
		rect.top = y_start;
		rect.bottom = rect.top + bar_height;
		mv_fillRect(&rect, mv_RGB2Color(255, 255, 255));
		i = i + 4;
	}
}
int do_display_string(cmd_tbl_t * cmdtp, int flag, int argc, char * const argv[])
{
	int i, ret = 0;
	ulong addr;
	char s[128];
	unsigned int pos_x,pos_y;
	unsigned int update_flag = 0;
	/* at least two arguments please */
	if (argc < 2)
		goto usage;
//	printf("argv[1] = %s,argv[2]= %s,argv[3] =%s,argv[4] =%s\n",argv[1],argv[2],argv[3],argv[4]);
	pos_x = (int)simple_strtoul(argv[1], NULL, 10);
	pos_y = (int)simple_strtoul(argv[2], NULL, 10);
	update_flag  = (int)simple_strtoul(argv[4], NULL, 10);
//	s=argv[3];
	if(update_flag == 0)
	{
		  sprintf((char *)s,"Update the %s OK!!",argv[3]);
		mv_textOut(pos_x, pos_y , s, mv_RGB2Color(0, 255, 0));
	}
	else if(update_flag == 1)
	{
		  sprintf((char *)s,"Update the %s ERROR!!",argv[3]);
		mv_textOut(pos_x, pos_y , s, mv_RGB2Color(255, 0, 0));
	}
	return 0;
usage:
	return CMD_RET_USAGE;
}
U_BOOT_CMD(
	disstring,	CONFIG_SYS_MAXARGS,	1,	do_display_string,
	"display string on LCD",
	"disstring Xpoint,Ypoint,string,colour\n"
);
