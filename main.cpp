#define _CRT_SECURE_NO_DEPRECATE 1
#define RW 0.3086f
#define GW 0.6094f
#define BW 0.0820f

#include <stdio.h>
#include <math.h>

#include "ScriptInterpreter.h"
#include "ScriptError.h"
#include "ScriptValue.h"

#include "resource.h"
#include "filter.h"

/* Global definitions */

static FilterDefinition *fd_VideocryptFilter;
void RGB2YUV(int r,int g,int b,int &Y,int &U,int &V);
void YUV2RGB(int Y,int U,int V,int &r,int &g,int &b);
int strToBin(const char * str);

/* Filter data definition */
typedef struct VCFilterData {
    int	 vcSeed;
	bool videocryptMode;
	bool modePAL;
	bool vcBorder;
	bool decDelay;
	int decMode;
} VCFilterData;


/* Videocrypt Filter Definition */

int RunProcVideocryptFilter(const FilterActivation *fa, const FilterFunctions *ff);
int StartProcVideocryptFilter(FilterActivation *fa, const FilterFunctions *ff);
int EndProcVideocryptFilter(FilterActivation *fa, const FilterFunctions *ff);
int ConfigProcVideocryptFilter(FilterActivation *fa, const FilterFunctions *ff, HWND hwnd);
void StringProcVideocryptFilter(const FilterActivation *fa, const FilterFunctions *ff, char *str);
int InitProcVideocryptFilter(FilterActivation *fa, const FilterFunctions *ff);
char *dsttemp;
char *tagline;



bool FssProcVideocryptFilter(FilterActivation *fa, const FilterFunctions *ff, char *buf, int buflen) {
	VCFilterData *mfd = (VCFilterData *)fa->filter_data;

	_snprintf(buf, buflen, "Config(%d, %d, %d, %d, %d)",
		mfd->vcSeed,
		mfd->modePAL,
		mfd->videocryptMode,
		mfd->vcBorder,
		mfd->decDelay);

	return true;
}

void ScriptConfigVideocryptFilter(IScriptInterpreter *isi, void *lpVoid, CScriptValue *argv, int argc) {
	FilterActivation *fa = (FilterActivation *)lpVoid;
	VCFilterData *mfd = (VCFilterData *)fa->filter_data;

	mfd->vcSeed			= argv[0].asInt();
	mfd->modePAL		= !!argv[1].asInt();
	mfd->videocryptMode	= !!argv[2].asInt();
	mfd->vcBorder		= !!argv[3].asInt();	
	mfd->decDelay		= !!argv[4].asInt();	
}

ScriptFunctionDef func_defs[]={
	{ (ScriptFunctionPtr)ScriptConfigVideocryptFilter, "Config", "0iiiii" },
	{ NULL },
};

CScriptObject script_objec={
	NULL, func_defs
};

int RunProcVideocryptFilter(const FilterActivation *fa, const FilterFunctions *ff)
{
	/* Pointer to MFD */
	VCFilterData *mfd = (VCFilterData *)fa->filter_data;
	
	/* Pointers to source and destination frame stores */
	char *src = (char *)fa->src.data;
	char *dst = (char *)fa->dst.data;

	/* Get line length */
	int dstline = fa->dst.w*sizeof(Pixel32)+fa->dst.modulo;
	int srcline = fa->src.w*sizeof(Pixel32)+fa->src.modulo;

	/* Destination frame width and height */
	int w = fa->dst.w; 
	int h = fa->dst.h;

	int vcoffset = (mfd->vcBorder == TRUE ? (int) (0.01 * w)*4 : 0);

	/* Source and target width address */
	int destx = fa->dst.w*sizeof(Pixel32);
	int srcx = fa->src.w*sizeof(Pixel32);

	/* PRNG parameters */
	int frame = fa->pfsi->lCurrentSourceFrame; // frame number
	int shift;

	/* Cut point locations for single cut mode */
	int min = (int) (srcx*.06);
	int max = (int) (srcx*.25);
	
	/* Define RGB/YUV variables */
	int r,g,b,Y,U,V;

	/* Pointer to source and destination structures */
	Pixel32 *dstc= fa->dst.data;
	Pixel32 *srcc= fa->src.data;

	if (mfd->videocryptMode) {

		/* 
		Tag format 
		==========
		First 7 high bits:	1101011 = Videocrypt
							1101010 = Trigger
		Rest of bits: randomised
		*/

		char seed[1024]="";
		int  _tags[2] = {0xFF,0x00};
		int tag[32];

		/* Check if you're encrypting an already encrypted video */
		// Grab tag line
		src +=srcline*(h-1);
		memcpy(tagline,src,srcx);
		src -=srcline*(h-1);

		// Read tag for Videocrypt tag - first 7 bits 
		tag[0] = (tagline[0] >= 0 ? 0 : 1);
		sprintf(seed, "%i", tag[0]);
		for (int i=1; i < 7;i++) {
			tag[i] = (tagline[((srcx/256)*4)*i] >= 0 ? 0 : 1);
			sprintf(seed, "%s%i", seed,tag[i]);
		}

		/* Check for magic number */
		if ( strToBin(seed) != 107 && strToBin(seed) != 106 )
		{
			int vctag[7]={0xFF,0xFF,0x00,0xFF,0x00,0xFF};

			if ( frame%37 == 0 ) 
			{
				vctag[6] = 0x00;
				sprintf(seed, "%s", "1101010");	
			}
			else
			{
				vctag[6] = 0xFF;
				sprintf(seed, "%s", "1101011");	
			}

			/* Generate seed for TAG LINE based on current frame and mfd->vcSeed value */
			srand((mfd->vcSeed+10)*((frame%256)+10)); if ((frame+2)%2 == 0) {	srand((rand()) + rand()); }	else { srand(rand() * (rand())); }

			for (int i=0; i < 7; i++)
				tag[i] = vctag[i];

			for (int i=7;i<32;i++)
			{
				// Random tag - black or white
				tag[i] = _tags[rand()%2];
				sprintf(seed, "%s%i", seed,(tag[i] == 0xFF ? 1 : 0));
			}

			memset(tagline,0x00,srcx);
			for (int i=0; i < 32; i++) 
			{
				memset(tagline+((srcx/256)*4)*i,tag[i],(srcx/256)*4);
			}
		}
		else 
		{
			/* Read tag for Videocrypt tag - remaining 25 bits */
			for (int i=7; i < 32;i++) {
				tag[i] = (tagline[((srcx/256)*4)*i] >= 0 ? 0 : 1);
				sprintf(seed, "%s%i", seed,tag[i]);
			}
		}

		if (mfd->modePAL) {
			/* Scramble chroma */

			/* Setup constants for rotation values */
			const double cosa[4] = {cos(45.0)*10000000,cos(135.0)*10000000,cos(225.0)*10000000,cos(315.0)*10000000};
			const double sina[4] = {sin(45.0)*10000000,sin(135.0)*10000000,sin(225.0)*10000000,sin(315.0)*10000000};


			/* Generate seed for PAL mode */

			srand(strToBin(seed)*mfd->vcSeed+10);

			for (int i=0; i<h-1; i++)
			{
				shift=(int) 4* (rand() %(max-min)+min + 1);
				shift = shift%w;			
				int H1 = (rand() % 3);
				int H2 = (rand() % 3);

				for (int x=0; x<w; x++)
				{
					r= (srcc[x]>>16)&0xff;
				  	g= (srcc[x]>> 8)&0xff;
				  	b= (srcc[x]    )&0xff;

					int H = (x < shift ? H1 : H2);

					/* Convert to YUV */
					RGB2YUV (r,g,b,Y,U,V);
									
					/* Rotate */
					int Ut = (int) ((((U-128)*10000000) * cosa[H] + ((V-128)*10000000) * sina[H])/10000000/10000000 + 128);
					int Vt = (int) ((((V-128)*10000000) * cosa[H] - ((U-128)*10000000) * sina[H])/10000000/10000000 + 128);

					/* "Line delay" (really, just reduce saturation of UV components) */
					Ut=(Ut+256)/3;
					Vt=(Vt+256)/3;

					/* Conver to RGB */
					YUV2RGB(Y,Ut,Vt,r,g,b);

					dstc[x] = (r << 16) | (g << 8) | (b);
				}
				
				/* Move onto next line */
				srcc+= fa->src.pitch>>2;
				dstc+= fa->dst.pitch>>2;
			}
		} 
		else
		{
			/* RGB mode - just copy src >> dst without change */
			memcpy(dst,src,srcx*h);
		}

		/* Cut and rotate */

		/* Generate seed for CUT/ROTATE */
		srand(strToBin(seed)*mfd->vcSeed+10);
		
 		for(int i=0; i < h-1; i++)
		{
			memcpy(dst,src,vcoffset);

			shift=(int) 4* (rand() %(max-min)+min + 1);
			shift = shift%(srcx-vcoffset);

			memcpy(dsttemp,dst+(shift+vcoffset),srcx-shift-vcoffset);
			memcpy(dsttemp+(srcx-shift-vcoffset),dst+vcoffset,shift);
			memcpy(dst+vcoffset,dsttemp,srcx-vcoffset);

			dst += dstline;
			src += srcline;
		}  

		/* Tag line */
		memcpy(dst,tagline,srcx);

	} 
	else // Decode start
	{
		int tag[32];
		char seed[1024]="";

		/* Grab a copy of tagline */
		src +=srcline*(h-1);
		memcpy(tagline,src,srcx);
		src -=srcline*(h-1);

		tag[0] = (tagline[0] >= 0 ? 0 : 1);
		sprintf(seed, "%i", tag[0]);

		/* Read tag for Videocrypt tag - first 7 bits */
		for (int i=1; i < 7;i++) {
			tag[i] = (tagline[((srcx/256)*4)*i] >= 0 ? 0 : 1);
			sprintf(seed, "%s%i", seed,tag[i]);
		}

		if ( strToBin(seed) == 106 && mfd->decMode > 0) mfd->decMode--;

		/* Check whether frame is tagged for Videocrypt (magic number 107) */
		if ((strToBin(seed) == 107 || strToBin(seed) == 106) && mfd->decMode < 2) {

			/* Read tag for Videocrypt tag - remaining 25 bits */
			for (int i=7; i < 32;i++) {
				tag[i] = (tagline[((srcx/256)*4)*i] >= 0 ? 0 : 1);
				sprintf(seed, "%s%i", seed,tag[i]);
			}

			/* Generate seed for CUT/ROTATE */
			int iseed = (strToBin(seed) != 0 ? strToBin(seed) : mfd->vcSeed+10*frame);

			if ( mfd->decMode == 1 )
			{
				srand(iseed*rand());
			}
			else
			{
				srand(iseed*mfd->vcSeed+10);
			}
			
 			for(int i=0; i < h-1; i++)
			{
				memcpy(dst,src,vcoffset);

				shift=(int) 4* (rand() %(max-min)+min + 1);
				shift = shift%(srcx-vcoffset);

				//shift = 752;

				memcpy(dsttemp,src+(srcx-shift-vcoffset),shift+vcoffset);
				memcpy(dsttemp+shift+vcoffset,src+vcoffset,srcx-shift-vcoffset);
				memcpy(dst+vcoffset,dsttemp+vcoffset,srcx-vcoffset);

				memcpy(dst+(srcx-vcoffset),src+(srcx-vcoffset),vcoffset);
				
				dst += dstline;
				src += srcline;
			} 

			if (mfd->modePAL && mfd->decMode == 0) {

				dst -= dstline*(h-1);
				src -= srcline*(h-1);
		
				/* Setup constants for rotation values */
				const double cosa[4] = {cos(-45.0)*10000000,cos(-135.0)*10000000,cos(-225.0)*10000000,cos(-315.0)*10000000};
				const double sina[4] = {sin(-45.0)*10000000,sin(-135.0)*10000000,sin(-225.0)*10000000,sin(-315.0)*10000000};

				/* Seed it */
				if ( mfd->decMode == 1 )
				{
					srand(iseed*rand());
				}
				else
				{
					srand(iseed*mfd->vcSeed+10);
				}

				for (int i=0; i<h-1; i++)
				{
					shift=(int) 4* (rand() %(max-min)+min + 1);
					shift = shift%w;			
					int H1 = (rand() % 3);
					int H2 = (rand() % 3);

					for (int x=0; x<w-(vcoffset/4); x++)
					{
						if (x > (vcoffset/4)-1) {
							r= (dstc[x]>>16)&0xff;
						  	g= (dstc[x]>> 8)&0xff;
						  	b= (dstc[x]    )&0xff;

							int H = (x < shift ? H1 : H2);

							/* Convert to YUV */
							RGB2YUV (r,g,b,Y,U,V);
											
							/* Rotate */
							int Ut = (int) ((((U-128)*10000000) * cosa[H] + ((V-128)*10000000) * sina[H])/10000000/10000000 + 128);
							int Vt = (int) ((((V-128)*10000000) * cosa[H] - ((U-128)*10000000) * sina[H])/10000000/10000000 + 128);

							dstc[x] = (Ut << 16) | (Y << 8) | (Vt);

						}
					}
					
					/* Move onto next line */
					dstc+= fa->dst.pitch>>2;
				}

				dstc-= (fa->dst.pitch>>2)*(h-1);

				/* Correct saturation */

				for (int i=0; i<h-1; i++)
				{
					for (int x=0; x<w; x++)
					{
						int Y=  (dstc[x]>> 8) & 0xff;
						int U= (dstc[x]>>16) & 0xff;
						int V= (dstc[x]    ) & 0xff;
						
						if (i < h-2) {
							int Ua= (dstc[x+(fa->dst.pitch>>2)]>>16) & 0xff;
							int Va= (dstc[x+(fa->dst.pitch>>2)]    ) & 0xff;

							U=(U+Ua)/2;
							V=(V+Va)/2;
						}

						YUV2RGB(Y,U,V,r,g,b);

						/* Saturation constant */
						const float s= 3.0f;

						const int SR= (int)(65536.0f*s);
						const int SG= (int)(65536.0f*s);
						const int SB= (int)(65536.0f*s);
						const int TR= (int)(65536.0f*RW*(1.0f-s));
						const int TG= (int)(65536.0f*GW*(1.0f-s));
						const int TB= (int)(65536.0f*BW*(1.0f-s));

						int t= TR*r+TG*g+TB*b+32768;
						r= (t+SR*r)>>16;
						g= (t+SG*g)>>16;
						b= (t+SB*b)>>16;
						if ((unsigned)r>255) r=(~r>>31)&255; 
						if ((unsigned)g>255) g=(~g>>31)&255; 
						if ((unsigned)b>255) b=(~b>>31)&255;

						dstc[x] = (r << 16) | (g << 8) | (b);
					}
					dstc+= fa->dst.pitch>>2;
				}

				for(int i=0; i < h-1; i++)
				{
					memcpy(dst,src,vcoffset);
					memcpy(dst+(srcx-vcoffset),src+(srcx-vcoffset),vcoffset);
					
					dst += dstline;
					src += srcline;
				} 
			}
			memcpy(dst,src,srcx);
		}
		else
		{
			memcpy(dst,src,fa->dst.size);
		}
	} // decode end
	return 0;
}

int EndProcVideocryptFilter(FilterActivation *fa, const FilterFunctions *ff)
{
	/* I should really add some deinitialisation code here to make it more memory friendly */
    return 0;
}

BOOL CALLBACK ecConfigDlgProc(HWND hdlg, UINT msg, WPARAM wParam, LPARAM lParam) {

	/* Pointer to MFD */
    VCFilterData *mfd = (VCFilterData *)GetWindowLong(hdlg, DWL_USER);

	/* Temporary storage for strings */
	char tmp[100];

    switch(msg) {
        case WM_INITDIALOG:
			/* Populate window with current values */

            SetWindowLong(hdlg, DWL_USER, lParam);
            mfd = (VCFilterData *)lParam;
            CheckDlgButton(hdlg, IDR_VCEncrypt, mfd->videocryptMode?BST_CHECKED:BST_UNCHECKED);
            CheckDlgButton(hdlg, IDR_VCDecrypt, mfd->videocryptMode?BST_UNCHECKED:BST_CHECKED);
			CheckDlgButton(hdlg, IDC_PAL, mfd->modePAL?BST_CHECKED:BST_UNCHECKED);
			CheckDlgButton(hdlg, IDC_VCBORDER, mfd->vcBorder?BST_CHECKED:BST_UNCHECKED);
			CheckDlgButton(hdlg, IDC_DECDELAY, mfd->decDelay?BST_CHECKED:BST_UNCHECKED);
			sprintf_s(tmp,"%i",mfd->vcSeed);
			SetDlgItemText(hdlg, IDC_VCSEED, tmp );

            return TRUE;

        case WM_COMMAND:
            switch(LOWORD(wParam)) {
            case IDOK:

				/* Store configured values */
				mfd->modePAL = !!IsDlgButtonChecked(hdlg, IDC_PAL);
				mfd->videocryptMode = !!IsDlgButtonChecked(hdlg, IDR_VCEncrypt);
				mfd->vcBorder=!!IsDlgButtonChecked(hdlg, IDC_VCBORDER);
				mfd->decDelay=!!IsDlgButtonChecked(hdlg, IDC_DECDELAY);
				GetDlgItemText(hdlg, IDC_VCSEED, tmp,10);
				mfd->vcSeed = atoi(tmp);
                EndDialog(hdlg, 0);
                return TRUE;
            case IDCANCEL:
				/* Exit without saving */
                EndDialog(hdlg, 1);
                return FALSE;
            }
            break;
    }

    return FALSE;
}


struct FilterDefinition filterDef_VideocryptFilter =
{
    NULL, NULL, NULL,				// next, prev, module
	"Videocrypt Encoder/Decoder",	// name
    "v1.00a - filter to emulate Videocrypt encoder and decoder as used on analogue satellite." ,    // desc
	"http://filmnet.plus",			// maker
    NULL,							// private_data
    sizeof(VCFilterData),			// inst_data_size
    InitProcVideocryptFilter,		// initProc
    NULL,							// deinitProc
    RunProcVideocryptFilter,		// runProc
    NULL,							// paramProc
	ConfigProcVideocryptFilter,		// configProc
    StringProcVideocryptFilter,		// stringProc
    StartProcVideocryptFilter,		// startProc
    EndProcVideocryptFilter,		// endProc
    &script_objec,					// script_obj
    FssProcVideocryptFilter,		// fssProc

};

int InitProcVideocryptFilter(FilterActivation *fa, const FilterFunctions *ff) {
	
	/* Pointer to MFD */
	VCFilterData *mfd = (VCFilterData *)fa->filter_data;

	/* Initialise default values */
	mfd->vcSeed = 9876;
	mfd->videocryptMode = 1;
	mfd->modePAL = 1;
	mfd->vcBorder = 0;
	srand((mfd->vcSeed+10));
	mfd->decDelay = FALSE;

	return 0;
}

/* Procedure to update text depending on options selected */
void StringProcVideocryptFilter(const FilterActivation *fa, const FilterFunctions *ff, char *str) {

	/* Pointer to MFD */
	VCFilterData *mfd = (VCFilterData *)fa->filter_data;
	
	/* Array of possible modes */
	const char *modes[2]={
		"decoder",
		"encoder",
    };

	const char *PAL[2]={
		"RGB",
		"PAL",
	};

	const char *border[2]={
		"OFF",
		"ON",
	};

	sprintf(str, " (%s, %s, seed %i, border %s, decode delay %s)", modes[mfd->videocryptMode], PAL[mfd->modePAL], mfd->vcSeed, border[mfd->vcBorder], border[mfd->decDelay]);
}

int StartProcVideocryptFilter(FilterActivation *fa, const FilterFunctions *ff)
{
	/* Pointer to MFD */
	VCFilterData *mfd = (VCFilterData *)fa->filter_data;

	/* Temporary line storage */
	dsttemp = (char*)malloc(fa->dst.w*sizeof(Pixel32));
	tagline = (char*)malloc(fa->dst.w*sizeof(Pixel32));

	mfd->decMode = mfd->decDelay == TRUE ? 2 : 0;
	
    return 0;
}

int ConfigProcVideocryptFilter(FilterActivation *fa, const FilterFunctions *ff, HWND hwnd)
{
	/* Open configuration box when filter is first loaded */
	return DialogBoxParam(fa->filter->module->hInstModule, MAKEINTRESOURCE(IDD_VCCONFIG), hwnd, ecConfigDlgProc, (LPARAM)fa->filter_data);
}

/* General routines to register filters */

extern "C" int __declspec(dllexport) __cdecl VirtualdubFilterModuleInit2(FilterModule *fm, const FilterFunctions *ff, int& vdfd_ver, int& vdfd_compat);
extern "C" void __declspec(dllexport) __cdecl VirtualdubFilterModuleDeinit(FilterModule *fm, const FilterFunctions *ff);

int __declspec(dllexport) __cdecl VirtualdubFilterModuleInit2(FilterModule *fm, const FilterFunctions *ff, int& vdfd_ver, int& vdfd_compat)
{
	if (!(fd_VideocryptFilter = ff->addFilter(fm, &filterDef_VideocryptFilter, sizeof(FilterDefinition))))
        return 1;

	vdfd_ver = VIRTUALDUB_FILTERDEF_VERSION;
	vdfd_compat = VIRTUALDUB_FILTERDEF_COMPATIBLE;

    return 0;
}

void __declspec(dllexport) __cdecl VirtualdubFilterModuleDeinit(FilterModule *fm, const FilterFunctions *ff)
{
	ff->removeFilter(fd_VideocryptFilter);
}

/* Conversion table courtesy of Emiliano Ferrari */
void RGB2YUV(int r,int g,int b,int &Y,int &U,int &V)
{
	// input:  r,g,b [0..255]
	// output: Y,U,V [0..255]
	const int Yr= (int)( 0.257*65536.0*255.0/219.0);
	const int Yg= (int)( 0.504*65536.0*255.0/219.0);
	const int Yb= (int)( 0.098*65536.0*255.0/219.0);
	const int Ur= (int)( 0.439*65536.0*255.0/223.0);
	const int Ug= (int)(-0.368*65536.0*255.0/223.0);
	const int Ub= (int)(-0.071*65536.0*255.0/223.0);	
	const int Vr= (int)(-0.148*65536.0*255.0/223.0);
	const int Vg= (int)(-0.291*65536.0*255.0/223.0);
	const int Vb= (int)( 0.439*65536.0*255.0/223.0);

	Y= ((int)(Yr * r + Yg * g + Yb * b+32767)>>16);
	U= ((int)(Ur * r + Ug * g + Ub * b+32767)>>16)+128;
	V= ((int)(Vr * r + Vg * g + Vb * b+32767)>>16)+128;
	if ((unsigned)U>255) U=(~U>>31)&0xff;
	if ((unsigned)V>255) V=(~V>>31)&0xff;
}

/* Conversion table courtesy of Emiliano Ferrari */
void YUV2RGB(int Y,int U,int V,int &r,int &g,int &b)
{
	// input:  Y,U,V [0..255]
	// output: r,g,b [0..255]
	const int YK= (int)(1.164*65536.0*219.0/255.0);
	const int k1= (int)(1.596*65536.0*223.0/255.0);
	const int k2= (int)(0.813*65536.0*223.0/255.0);
	const int k3= (int)(2.018*65536.0*223.0/255.0);
	const int k4= (int)(0.391*65536.0*223.0/255.0);

	Y*= YK;
	U-= 128;
	V-= 128;

	r= (Y+k1*U+32768)>>16;
	g= (Y-k2*U-k4*V+32768)>>16;
	b= (Y+k3*V+32768)>>16;

	if ((unsigned)r>255) r=(~r>>31)&0xff;
	if ((unsigned)g>255) g=(~g>>31)&0xff;
	if ((unsigned)b>255) b=(~b>>31)&0xff;
}

int strToBin(const char * str)
{
    int val = 0;

    while (*str != '\0')
        val = 2 * val + (*str++ - '0');
    return val;
}
