/*
Copyright (C) 1997-2001 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
/*
** GLW_IMP.C
**
** This file contains ALL Win32 specific stuff having to do with the
** OpenGL refresh.  When a port is being made the following functions
** must be implemented by the port:
**
** GLimp_EndFrame
** GLimp_Init
** GLimp_Shutdown
** GLimp_SwitchFullscreen
**
*/
#include <float.h>
#include <windows.h>
#include "../ref_gl/gl_local.h"
#include "glw_win.h"
#include "winquake.h"
#include "wglext.h"

//static qboolean GLimp_SwitchFullscreen( int width, int height );
qboolean GLimp_InitGL (void);

glwstate_t glw_state;

extern cvar_t *vid_fullscreen;
extern cvar_t *vid_ref;
extern cvar_t *vid_forcedrefresh;
extern cvar_t *vid_nowgl;

DEVMODE		originalDesktopMode;
DEVMODE		fullScreenMode;

qboolean	usingDesktopSettings;

static qboolean VerifyDriver( void )
{
	char buffer[1024];

	Q_strncpy( buffer, qglGetString( GL_RENDERER ), sizeof(buffer)-1);
	strlwr( buffer );
	if ( strcmp( buffer, "gdi generic" ) == 0 )
		if ( !glw_state.mcd_accelerated )
			return false;
	return true;
}

/*
** VID_CreateWindow
*/
#define	WINDOW_CLASS_NAME	"Quake 2"
#define OPENGL_CLASS		"OpenGLDummyPFDWindow"

qboolean VID_CreateWindow( int width, int height, qboolean fullscreen )
{
	WNDCLASS		wc;
	RECT			r;
	cvar_t			*vid_xpos, *vid_ypos;
	int				stylebits;
	int				x, y, w, h;
	int				exstyle;

	/* Register the frame class */
    wc.style         = 0;
    wc.lpfnWndProc   = (WNDPROC)glw_state.wndproc;
    wc.cbClsExtra    = 0;
    wc.cbWndExtra    = 0;
    wc.hInstance     = glw_state.hInstance;
    wc.hIcon         = 0;
    wc.hCursor       = LoadCursor (NULL,IDC_ARROW);
	wc.hbrBackground = (HBRUSH)COLOR_GRAYTEXT;
    wc.lpszMenuName  = 0;
    wc.lpszClassName = WINDOW_CLASS_NAME;

    if (!RegisterClass (&wc) )
		ri.Sys_Error (ERR_FATAL, "Couldn't register window class (%d)", GetLastError());

	if (fullscreen)
	{
		exstyle = WS_EX_TOPMOST;
		stylebits = WS_POPUP|WS_VISIBLE;
	}
	else
	{
		exstyle = 0;
		stylebits = WINDOW_STYLE|WS_SYSMENU|WS_MINIMIZEBOX|WS_MAXIMIZEBOX;
	}

	r.left = 0;
	r.top = 0;
	r.right  = width;
	r.bottom = height;

	AdjustWindowRect (&r, stylebits, FALSE);

	w = r.right - r.left;
	h = r.bottom - r.top;

	if (fullscreen)
	{
		x = 0;
		y = 0;
	}
	else
	{
		vid_xpos = ri.Cvar_Get ("vid_xpos", "0", 0);
		vid_ypos = ri.Cvar_Get ("vid_ypos", "0", 0);
		x = Q_ftol(vid_xpos->value);
		y = Q_ftol(vid_ypos->value);
	}

	glw_state.hWnd = CreateWindowEx (
		 exstyle, 
		 WINDOW_CLASS_NAME,
		 "Quake 2",
		 stylebits,
		 x, y, w, h,
		 NULL,
		 NULL,
		 glw_state.hInstance,
		 NULL);

	if (!glw_state.hWnd)
		ri.Sys_Error (ERR_FATAL, "Couldn't create window");
	
	ShowWindow( glw_state.hWnd, SW_SHOW );
	UpdateWindow( glw_state.hWnd );

	// init all the gl stuff for the window
	if (!GLimp_InitGL ())
	{
		ri.Con_Printf( PRINT_ALL, "VID_CreateWindow() - GLimp_InitGL failed\n");
		return false;
	}

	SetForegroundWindow( glw_state.hWnd );
	SetFocus( glw_state.hWnd );

	// let the sound and input subsystems know about the new window
	ri.Vid_NewWindow (width, height);

	return true;
}


/*
** GLimp_SetMode
*/
rserr_t GLimp_SetMode( unsigned int *pwidth, unsigned int *pheight, int mode, qboolean fullscreen )
{
	int width, height;
	const char *win_fs[] = { "W", "FS" };

	ri.Con_Printf( PRINT_ALL, "Initializing OpenGL display\n");

	ri.Con_Printf (PRINT_ALL, "...setting mode %d:", mode );

	if ( !ri.Vid_GetModeInfo( &width, &height, mode ) )
	{
		ri.Con_Printf( PRINT_ALL, " invalid mode\n" );
		return rserr_invalid_mode;
	}

	if (FLOAT_NE_ZERO(gl_forcewidth->value))
		width = (int)gl_forcewidth->value;

	if (FLOAT_NE_ZERO(gl_forceheight->value))
		height = (int)gl_forceheight->value;

	ri.Con_Printf( PRINT_ALL, " %d %d %s\n", width, height, win_fs[fullscreen] );

	// destroy the existing window
	if (glw_state.hWnd)
	{
		GLimp_Shutdown ();
	}

	// do a CDS if needed
	if ( fullscreen )
	{
		DEVMODE dm;

		EnumDisplaySettings (NULL, ENUM_CURRENT_SETTINGS, &originalDesktopMode);

		ri.Con_Printf( PRINT_ALL, "...attempting fullscreen\n" );

		memset( &dm, 0, sizeof( dm ) );

		dm.dmSize = sizeof( dm );

		dm.dmPelsWidth  = width;
		dm.dmPelsHeight = height;
		dm.dmFields     = DM_PELSWIDTH | DM_PELSHEIGHT;

		//r1: allow refresh overriding
		if (FLOAT_NE_ZERO(vid_forcedrefresh->value))
		{
			dm.dmFields |= DM_DISPLAYFREQUENCY;
			dm.dmDisplayFrequency = Q_ftol(vid_forcedrefresh->value);
		}

		if ( FLOAT_NE_ZERO(gl_bitdepth->value) )
		{
			dm.dmBitsPerPel = Q_ftol(gl_bitdepth->value);
			dm.dmFields |= DM_BITSPERPEL;
			ri.Con_Printf( PRINT_ALL, "...using gl_bitdepth of %d\n", ( int ) gl_bitdepth->value );
		}
		else
		{
			ri.Con_Printf( PRINT_ALL, "...using desktop display depth of %d\n", originalDesktopMode.dmBitsPerPel );
		}

		ri.Con_Printf( PRINT_ALL, "...calling CDS: " );
		if ( ChangeDisplaySettings( &dm, CDS_FULLSCREEN ) == DISP_CHANGE_SUCCESSFUL )
		{
			*pwidth = width;
			*pheight = height;

			gl_state.fullscreen = true;

			ri.Con_Printf( PRINT_ALL, "ok\n" );

			if ( !VID_CreateWindow (width, height, true) )
				return rserr_invalid_mode;

			EnumDisplaySettings (NULL, ENUM_CURRENT_SETTINGS, &fullScreenMode);
			return rserr_ok;
		}
		else
		{
			*pwidth = width;
			*pheight = height;

			ri.Con_Printf( PRINT_ALL, "failed\n" );

			ri.Con_Printf( PRINT_ALL, "...calling CDS assuming dual monitors:" );

			dm.dmPelsWidth = width * 2;
			dm.dmPelsHeight = height;
			dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;

			if ( FLOAT_NE_ZERO(gl_bitdepth->value) )
			{
				dm.dmBitsPerPel = Q_ftol(gl_bitdepth->value);
				dm.dmFields |= DM_BITSPERPEL;
			}

			/*
			** our first CDS failed, so maybe we're running on some weird dual monitor
			** system 
			*/
			if ( ChangeDisplaySettings( &dm, CDS_FULLSCREEN ) != DISP_CHANGE_SUCCESSFUL )
			{
				ri.Con_Printf( PRINT_ALL, " failed\n" );

				ri.Con_Printf( PRINT_ALL, "...setting windowed mode\n" );

				ChangeDisplaySettings( 0, 0 );

				*pwidth = width;
				*pheight = height;
				gl_state.fullscreen = false;
				if ( !VID_CreateWindow (width, height, false) )
					return rserr_invalid_mode;
				return rserr_invalid_fullscreen;
			}
			else
			{
				ri.Con_Printf( PRINT_ALL, " ok\n" );
				if ( !VID_CreateWindow (width, height, true) )
					return rserr_invalid_mode;

				EnumDisplaySettings (NULL, ENUM_CURRENT_SETTINGS, &fullScreenMode);
				gl_state.fullscreen = true;
				return rserr_ok;
			}
		}
	}
	else
	{
		ri.Con_Printf( PRINT_ALL, "...setting windowed mode\n" );

		ChangeDisplaySettings( 0, 0 );

		*pwidth = width;
		*pheight = height;
		gl_state.fullscreen = false;
		if ( !VID_CreateWindow (width, height, false) )
			return rserr_invalid_mode;
	}

	return rserr_ok;
}

/*
** GLimp_Shutdown
**
** This routine does all OS specific shutdown procedures for the OpenGL
** subsystem.  Under OpenGL this means NULLing out the current DC and
** HGLRC, deleting the rendering context, and releasing the DC acquired
** for the window.  The state structure is also nulled out.
**
*/
void GLimp_Shutdown( void )
{


#ifdef USE_MSGLOG
  if(hMsgLog) FreeLibrary(hMsgLog);
#endif

	if ( qwglMakeCurrent && !qwglMakeCurrent( NULL, NULL ) )
		ri.Con_Printf( PRINT_ALL, "ref_gl::R_Shutdown() - wglMakeCurrent failed\n");
	if ( glw_state.hGLRC )
	{
		if (  qwglDeleteContext && !qwglDeleteContext( glw_state.hGLRC ) )
			ri.Con_Printf( PRINT_ALL, "ref_gl::R_Shutdown() - wglDeleteContext failed\n");
		glw_state.hGLRC = NULL;
	}
	if (glw_state.hDC)
	{
		if ( !ReleaseDC( glw_state.hWnd, glw_state.hDC ) )
			ri.Con_Printf( PRINT_ALL, "ref_gl::R_Shutdown() - ReleaseDC failed\n" );
		glw_state.hDC   = NULL;
	}

	if (glw_state.hWnd)
	{
		ShowWindow (glw_state.hWnd, SW_HIDE);
		DestroyWindow (	glw_state.hWnd );
		glw_state.hWnd = NULL;
	}

	if ( glw_state.log_fp )
	{
		fclose( glw_state.log_fp );
		glw_state.log_fp = 0;
	}

	UnregisterClass (WINDOW_CLASS_NAME, glw_state.hInstance);
	UnregisterClass (OPENGL_CLASS, glw_state.hInstance);

	if ( gl_state.fullscreen )
	{
		ChangeDisplaySettings( 0, 0 );
		gl_state.fullscreen = false;
	}
}


/*
** GLimp_Init
**
** This routine is responsible for initializing the OS specific portions
** of OpenGL.  Under Win32 this means dealing with the pixelformats and
** doing the wgl interface stuff.
*/
qboolean GLimp_Init( void *hinstance, void *wndproc )
{
#define OSR2_BUILD_NUMBER 1111

	OSVERSIONINFO	vinfo;

	_controlfp( _PC_24, _MCW_PC );

	vinfo.dwOSVersionInfoSize = sizeof(vinfo);

	glw_state.allowdisplaydepthchange = false;

	if ( GetVersionEx( &vinfo) )
	{
		if ( vinfo.dwMajorVersion > 4 )
		{
			glw_state.allowdisplaydepthchange = true;
		}
		else if ( vinfo.dwMajorVersion == 4 )
		{
			if ( vinfo.dwPlatformId == VER_PLATFORM_WIN32_NT )
			{
				glw_state.allowdisplaydepthchange = true;
			}
			else if ( vinfo.dwPlatformId == VER_PLATFORM_WIN32_WINDOWS )
			{
				if ( LOWORD( vinfo.dwBuildNumber ) >= OSR2_BUILD_NUMBER )
				{
					glw_state.allowdisplaydepthchange = true;
				}
			}
		}
	}
	else
	{
		ri.Con_Printf( PRINT_ALL, "GLimp_Init() - GetVersionEx failed\n" );
		return false;
	}

	glw_state.hInstance = ( HINSTANCE ) hinstance;
	glw_state.wndproc = wndproc;

	return true;
}

// Define entry points
#define WGL_SAMPLE_BUFFERS_ARB             0x2041
#define WGL_SAMPLES_ARB                    0x2042

// WGL_ARB_extensions_string
PFNWGLGETEXTENSIONSSTRINGARBPROC     wglGetExtensionsStringARB;

PFNWGLGETEXTENSIONSSTRINGEXTPROC     wglGetExtensionsStringEXT;

// WGL_ARB_pixel_format
PFNWGLGETPIXELFORMATATTRIBIVARBPROC  wglGetPixelFormatAttribivARB;
PFNWGLGETPIXELFORMATATTRIBFVARBPROC  wglGetPixelFormatAttribfvARB;
PFNWGLCHOOSEPIXELFORMATARBPROC       wglChoosePixelFormatARB;

qboolean _is_multisample;

// Determine if an OpenGL extension is supported.
int _glExtensionSupported(const char *extension)
{
    static const GLubyte *extensions = NULL;
    const GLubyte *start;
    GLubyte *where, *terminator;
    
    extensions = qglGetString(GL_EXTENSIONS);

    // Extension names should not have spaces.
    where = (GLubyte *) strchr(extension, ' ');
    if (where || *extension == '\0')
        return 0;

    // It takes a bit of care to be fool-proof about parsing the
    // OpenGL extensions string.  Don't be fooled by sub-strings,
    // etc.
    start = extensions;
    for (;;)
    {
        where = (GLubyte *) strstr((const char *) start, extension);
        if (!where)
            break;
        terminator = where + strlen(extension);
        if (where == start || *(where - 1) == ' ')
            if (*terminator == ' ' || *terminator == '\0')
                return 1;

        start = terminator;
    }
    return 0;
}

// Determine if an OpenGL WGL extension is supported.
//
// NOTE:  This routine uses wglGetProcAddress so this routine REQUIRES
// that the calling thread is bound to a hardware-accelerated OpenGL
// rendering context.
int _wglExtensionSupported(const char *extension)
{
    if (wglGetExtensionsStringARB || wglGetExtensionsStringEXT) 
    {
        static const GLubyte *extensions = NULL;
        const GLubyte *start;
        GLubyte *where, *terminator;
  
        // Extension names should not have spaces.
        where = (GLubyte *) strchr(extension, ' ');
        if (where || *extension == '\0')
            return 0;
  
        if (!extensions)
        {
            HDC hdc = GetDC(0);
            if (wglGetExtensionsStringARB)
                extensions = (const GLubyte *) wglGetExtensionsStringARB(hdc);
            else
                extensions = (const GLubyte *) wglGetExtensionsStringEXT();
            ReleaseDC(0, hdc);
			//ri.Con_Printf (PRINT_ALL, "wglExtensions: %s\n", extensions);
        }

        // It takes a bit of care to be fool-proof about parsing the
        // OpenGL extensions string.  Don't be fooled by sub-strings,
        // etc.
        start = extensions;
        for (;;) {
            where = (GLubyte *) strstr((const char *) start, extension);
            if (!where)
                break;
            terminator = where + strlen(extension);
            if (where == start || *(where - 1) == ' ')
                if (*terminator == ' ' || *terminator == '\0')
                    return 1;

            start = terminator;
        }
    }
    else
	{
        ri.Sys_Error (ERR_FATAL, "WGL extension string not found!");
	}

    return 0;
}

qboolean init_extensions()
{
    if (_wglExtensionSupported("WGL_ARB_extensions_string") == 0)
    {
        if (_glExtensionSupported("WGL_EXT_extensions_string") == 0)
        {
            ri.Con_Printf (PRINT_ALL, "init_extensions: Neither WGL_ARB_extensions_string nor WGL_EXT_extensions_string found!");
            return false;
        }
    }

    if (_wglExtensionSupported("WGL_ARB_multisample") && gl_ext_multisample->value)
    {
        _is_multisample = true;
    }
    else 
    {
		if (FLOAT_NE_ZERO(gl_ext_multisample->value))
			ri.Con_Printf (PRINT_ALL, "WGL_ARB_multisample not found.\n");
		ri.Cvar_Set ("gl_ext_multisample", "0");
        _is_multisample = false;
    }
    
    if (_wglExtensionSupported("WGL_ARB_pixel_format"))
	{
        wglGetPixelFormatAttribivARB = (PFNWGLGETPIXELFORMATATTRIBIVARBPROC)qwglGetProcAddress("wglGetPixelFormatAttribivARB");
        wglGetPixelFormatAttribfvARB = (PFNWGLGETPIXELFORMATATTRIBFVARBPROC)qwglGetProcAddress("wglGetPixelFormatAttribfvARB");
        wglChoosePixelFormatARB = (PFNWGLCHOOSEPIXELFORMATARBPROC)qwglGetProcAddress("wglChoosePixelFormatARB");
	}
	else
    {
		ri.Con_Printf (PRINT_ALL, "init_extensions: WGL_ARB_pixel_format not found!");
        return false;
    }
    
    return true;
}

// don't do anything
static LRESULT CALLBACK StupidOpenGLProc(HWND hWnd,UINT msg,WPARAM wParam,LPARAM lParam)
{
  return DefWindowProc(hWnd,msg,wParam,lParam);
}

// Registers the window classes
BOOL RegisterOpenGLWindow(HINSTANCE hInst)
{
  WNDCLASSEX wcex;

  // Initialize our Window class
  wcex.cbSize = sizeof(wcex);
  if (GetClassInfoEx(hInst, OPENGL_CLASS, &wcex))
	return TRUE;

  // register main one
  ZeroMemory(&wcex,sizeof(wcex));

  // now the stupid one
  wcex.cbSize			= sizeof(wcex);
  wcex.style			= CS_OWNDC;
  wcex.cbWndExtra		= 0; /* space for our pointer */
  wcex.lpfnWndProc		= StupidOpenGLProc;
  wcex.hbrBackground	= NULL;
  wcex.hInstance		= hInst;
  wcex.hCursor			= LoadCursor(NULL,IDC_ARROW);
  wcex.lpszClassName	= OPENGL_CLASS;

  return RegisterClassEx(&wcex);
}

qboolean init_regular (void)
{
    PIXELFORMATDESCRIPTOR pfd = 
	{
		sizeof(PIXELFORMATDESCRIPTOR),	// size of this pfd
		1,								// version number
		PFD_DRAW_TO_WINDOW |			// support window
		PFD_SUPPORT_OPENGL |			// support OpenGL
		PFD_DOUBLEBUFFER,				// double buffered
		PFD_TYPE_RGBA,					// RGBA type
		24,								// 24-bit color depth
		0, 0, 0, 0, 0, 0,				// color bits ignored
		0,								// no alpha buffer
		0,								// shift bit ignored
		0,								// no accumulation buffer
		0, 0, 0, 0, 					// accum bits ignored
		32,								// 32-bit z-buffer	
		0,								// no stencil buffer
		0,								// no auxiliary buffer
		PFD_MAIN_PLANE,					// main layer
		0,								// reserved
		0, 0, 0							// layer masks ignored
    };
    int  pixelformat;

#ifdef STEREO_SUPPORT
	cvar_t *stereo;
	
	stereo = ri.Cvar_Get( "cl_stereo", "0", 0 );

	/*
	** set PFD_STEREO if necessary
	*/
	if ( FLOAT_NE_ZERO(stereo->value))
	{
		ri.Con_Printf( PRINT_ALL, "...attempting to use stereo\n" );
		pfd.dwFlags |= PFD_STEREO;
		gl_state.stereo_enabled = true;
	}
	else
	{
		gl_state.stereo_enabled = false;
	}
#endif

	/*
	** figure out if we're running on a minidriver or not
	*/
	if ( strstr( gl_driver->string, "opengl32" ) != 0 )
		glw_state.minidriver = false;
	else
		glw_state.minidriver = true;

	/*
	** Get a DC for the specified window
	*/
	if ( glw_state.hDC != NULL )
		ri.Con_Printf( PRINT_ALL, "GLimp_Init() - non-NULL DC exists\n" );

    if ( ( glw_state.hDC = GetDC( glw_state.hWnd ) ) == NULL )
	{
		ri.Con_Printf( PRINT_ALL, "GLimp_Init() - GetDC failed\n" );
		return false;
	}

	if ( glw_state.minidriver )
	{
		if ( (pixelformat = qwglChoosePixelFormat( glw_state.hDC, &pfd)) == 0 )
		{
			ri.Con_Printf (PRINT_ALL, "GLimp_Init() - qwglChoosePixelFormat failed\n");
			return false;
		}
		if ( qwglSetPixelFormat( glw_state.hDC, pixelformat, &pfd) == FALSE )
		{
			ri.Con_Printf (PRINT_ALL, "GLimp_Init() - qwglSetPixelFormat failed\n");
			return false;
		}
		qwglDescribePixelFormat( glw_state.hDC, pixelformat, sizeof( pfd ), &pfd );
	}
	else
	{
		if ( ( pixelformat = ChoosePixelFormat( glw_state.hDC, &pfd)) == 0 )
		{
			ri.Con_Printf (PRINT_ALL, "GLimp_Init() - ChoosePixelFormat failed\n");
			return false;
		}
		if ( SetPixelFormat( glw_state.hDC, pixelformat, &pfd) == FALSE )
		{
			ri.Con_Printf (PRINT_ALL, "GLimp_Init() - SetPixelFormat failed\n");
			return false;
		}
		DescribePixelFormat( glw_state.hDC, pixelformat, sizeof( pfd ), &pfd );

		if ( !( pfd.dwFlags & PFD_GENERIC_ACCELERATED ) )
		{
			extern cvar_t *gl_allow_software;

			if ( FLOAT_NE_ZERO(gl_allow_software->value) )
				glw_state.mcd_accelerated = true;
			else
				glw_state.mcd_accelerated = false;
		}
		else
		{
			glw_state.mcd_accelerated = true;
		}
	}

#ifdef STEREO_SUPPORT
	/*
	** report if stereo is desired but unavailable
	*/
	if ( !( pfd.dwFlags & PFD_STEREO ) && ( FLOAT_NE_ZERO(stereo->value)) ) 
	{
		ri.Con_Printf( PRINT_ALL, "...failed to select stereo pixel format\n" );
		ri.Cvar_SetValue( "cl_stereo", 0 );
		gl_state.stereo_enabled = false;
	}
#endif

	/*
	** startup the OpenGL subsystem by creating a context and making
	** it current
	*/
	if ( ( glw_state.hGLRC = qwglCreateContext( glw_state.hDC ) ) == 0 )
	{
		ri.Con_Printf (PRINT_ALL, "GLimp_Init() - qwglCreateContext failed\n");

		goto fail;
	}

    if ( !qwglMakeCurrent( glw_state.hDC, glw_state.hGLRC ) )
	{
		ri.Con_Printf (PRINT_ALL, "GLimp_Init() - qwglMakeCurrent failed\n");

		goto fail;
	}

	if ( !VerifyDriver() )
	{
		ri.Con_Printf( PRINT_ALL, "GLimp_Init() - no hardware acceleration detected\n" );
		goto fail;
	}

	/*
	** print out PFD specifics 
	*/
	ri.Con_Printf( PRINT_ALL, "GL PFD: color(%d-bits) Z(%d-bit)\n", ( int ) pfd.cColorBits, ( int ) pfd.cDepthBits );

	return true;

fail:
	if ( glw_state.hGLRC )
	{
		qwglDeleteContext( glw_state.hGLRC );
		glw_state.hGLRC = NULL;
	}

	if ( glw_state.hDC )
	{
		ReleaseDC( glw_state.hWnd, glw_state.hDC );
		glw_state.hDC = NULL;
	}
	return false;
}
/*
qboolean init_regular (void)
{
    PIXELFORMATDESCRIPTOR pfd = 
	{
		sizeof(PIXELFORMATDESCRIPTOR),	// size of this pfd
		1,								// version number
		PFD_DRAW_TO_WINDOW |			// support window
		PFD_SUPPORT_OPENGL |			// support OpenGL
		PFD_GENERIC_ACCELERATED |		// accelerated
		PFD_DOUBLEBUFFER,				// double buffered
		PFD_TYPE_RGBA,					// RGBA type
		24,								// 24-bit color depth
		0, 0, 0, 0, 0, 0,				// color bits ignored
		0,								// no alpha buffer
		0,								// shift bit ignored
		0,								// no accumulation buffer
		0, 0, 0, 0, 					// accum bits ignored
		32,								// 32-bit z-buffer	
		0,								// no stencil buffer
		0,								// no auxiliary buffer
		PFD_MAIN_PLANE,					// main layer
		0,								// reserved
		0, 0, 0							// layer masks ignored
    };

    int  pixelformat;

	if ( ( glw_state.hDC = GetDC( glw_state.hWnd ) ) == NULL )
	{
		ri.Con_Printf( PRINT_ALL, "�����������̨������� GetDC failed\n" );
		return false;
	}

	if ( ( pixelformat = ChoosePixelFormat( glw_state.hDC, &pfd)) == 0 )
	{
		ri.Con_Printf (PRINT_ALL, "�����������̨������� ChoosePixelFormat failed\n");
		return false;
	}

	if ( SetPixelFormat( glw_state.hDC, pixelformat, &pfd) == FALSE )
	{
		ri.Con_Printf (PRINT_ALL, "�����������̨������� SetPixelFormat failed\n");
		return false;
	}

	DescribePixelFormat( glw_state.hDC, pixelformat, sizeof( pfd ), &pfd );

	if ( !( pfd.dwFlags & PFD_GENERIC_ACCELERATED ) )
	{
		extern cvar_t *gl_allow_software;

		if ( gl_allow_software->value )
			glw_state.mcd_accelerated = true;
		else
			glw_state.mcd_accelerated = false;
	}
	else
	{
		glw_state.mcd_accelerated = true;
	}

#ifdef STEREO_SUPPORT
	if ( !( pfd.dwFlags & PFD_STEREO ) && ( stereo->value != 0 ) ) 
	{
		ri.Con_Printf( PRINT_ALL, "...failed to select stereo pixel format\n" );
		ri.Cvar_SetValue( "cl_stereo", 0 );
		gl_state.stereo_enabled = false;
	}
#endif

	if ( ( glw_state.hGLRC = qwglCreateContext( glw_state.hDC ) ) == 0 )
	{
		ri.Con_Printf (PRINT_ALL, "�����������̨������� qwglCreateContext failed\n");

		goto fail;
	}

	if ( !qwglMakeCurrent( glw_state.hDC, glw_state.hGLRC ) )
	{
		ri.Con_Printf (PRINT_ALL, "�����������̨������� qwglMakeCurrent failed\n");

		goto fail;
	}

	return true;

fail:
	if ( glw_state.hGLRC )
	{
		qwglDeleteContext( glw_state.hGLRC );
		glw_state.hGLRC = NULL;
	}

	if ( glw_state.hDC )
	{
		ReleaseDC( glw_state.hWnd, glw_state.hDC );
		glw_state.hDC = NULL;
	}
	return false;
}*/

qboolean GLimp_InitGL (void)
{
    PIXELFORMATDESCRIPTOR pfd = 
	{
		sizeof(PIXELFORMATDESCRIPTOR),	// size of this pfd
		1,								// version number
		PFD_DRAW_TO_WINDOW |			// support window
		PFD_SUPPORT_OPENGL |			// support OpenGL
		PFD_GENERIC_ACCELERATED |		// accelerated
		PFD_DOUBLEBUFFER,				// double buffered
		PFD_TYPE_RGBA,					// RGBA type
		24,								// 24-bit color depth
		0, 0, 0, 0, 0, 0,				// color bits ignored
		0,								// no alpha buffer
		0,								// shift bit ignored
		0,								// no accumulation buffer
		0, 0, 0, 0, 					// accum bits ignored
		32,								// 32-bit z-buffer	
		0,								// no stencil buffer
		0,								// no auxiliary buffer
		PFD_MAIN_PLANE,					// main layer
		0,								// reserved
		0, 0, 0							// layer masks ignored
    };

    int  pixelformat;
#ifdef STEREO_SUPPORT
	cvar_t *stereo;
#endif

#ifdef STEREO_SUPPORT
	stereo = ri.Cvar_Get( "cl_stereo", "0", 0 );

	/*
	** set PFD_STEREO if necessary
	*/
	if ( FLOAT_NE_ZERO(stereo->value))
	{
		ri.Con_Printf( PRINT_ALL, "...attempting to use stereo\n" );
		pfd.dwFlags |= PFD_STEREO;
		gl_state.stereo_enabled = true;
	}
	else
	{
		gl_state.stereo_enabled = false;
	}
#endif

	if (FLOAT_NE_ZERO(vid_nowgl->value))
		return init_regular ();

	/*
	** figure out if we're running on a minidriver or not
	*/
	if ( strstr( gl_driver->string, "opengl32" ) != 0 )
		glw_state.minidriver = false;
	else
		glw_state.minidriver = true;

	/*
	** Get a DC for the specified window
	*/
	if ( glw_state.hDC != NULL )
		ri.Con_Printf( PRINT_ALL, "�����������̨������� non-NULL DC exists\n" );

	if ( glw_state.minidriver )
	{
		ri.Con_Printf (PRINT_ALL, "WARNING: MINIDRIVER DETECTED! r1gl has not been tested with minidrivers! it may crash or completely fail to work. if you are running on 3dfx or other non-opengl compatible hardware, set vid_nowgl 1 before loading r1gl, otherwise be sure gl_driver is set to opengl32\n");
		//ri.Sys_Error (ERR_DROP, "unsupported gl_driver");
		if ( (pixelformat = qwglChoosePixelFormat( glw_state.hDC, &pfd)) == 0 )
		{
			ri.Con_Printf (PRINT_ALL, "�����������̨������� qwglChoosePixelFormat failed\n");
			return false;
		}
		if ( qwglSetPixelFormat( glw_state.hDC, pixelformat, &pfd) == FALSE )
		{
			ri.Con_Printf (PRINT_ALL, "�����������̨������� qwglSetPixelFormat failed\n");
			return false;
		}
		qwglDescribePixelFormat( glw_state.hDC, pixelformat, sizeof( pfd ), &pfd );
	}

	{
		WORD ramps[3][256];
		DEVMODE dm;
		HDC hdDesk;
		memset( &dm, 0, sizeof( dm ) );
		hdDesk = GetDC( GetDesktopWindow() );

		dm.dmSize = sizeof( dm );
		dm.dmBitsPerPel = GetDeviceCaps( hdDesk, BITSPIXEL );
		dm.dmFields     = DM_BITSPERPEL;

		if (GetDeviceGammaRamp (hdDesk, ramps))
		{
			if (ramps[0][0] > 0 || ramps[1][0] > 0 || ramps[2][0] > 0)
				ri.Cvar_Get ("vid_hwgamma", va ("GDGR: %d-%d-%d", ramps[0][0], ramps[1][0], ramps[2][0]), CVAR_NOSET);
		}

		ReleaseDC( GetDesktopWindow(), hdDesk );

		if (FLOAT_LE_ZERO(gl_colorbits->value)) 
			ri.Cvar_Set ("gl_colorbits", dm.dmBitsPerPel == 32 ? "24" : "16");

		if (FLOAT_LE_ZERO(gl_depthbits->value))
			ri.Cvar_Set ("gl_depthbits", dm.dmBitsPerPel == 32 ? "24" : "16");

		if (!*gl_alphabits->string)
			ri.Cvar_Set ("gl_alphabits", dm.dmBitsPerPel == 32 ? "8" : "0");

		if (!*gl_stencilbits->string)
			ri.Cvar_Set ("gl_stencilbits", dm.dmBitsPerPel == 32 ? "8" : "0");
	}

	if (gl_colorbits->value < 24) {
		if (FLOAT_NE_ZERO(gl_alphabits->value)) {
			ri.Con_Printf (PRINT_ALL, "GLimp_InitGL() - disabling gl_alphabits with colorbits %d\n", (int)gl_colorbits->value);
			ri.Cvar_Set ("gl_alphabits", "0");
		}
		if (FLOAT_NE_ZERO(gl_stencilbits->value)) {
			ri.Con_Printf (PRINT_ALL, "GLimp_InitGL() - disabling gl_stencilbits with colorbits %d\n", (int)gl_colorbits->value);
			ri.Cvar_Set ("gl_stencilbits", "0");
		}
	}

	RegisterOpenGLWindow (glw_state.hInstance);

	//if (1)
	{
		int     iAttributes[30];
		float   fAttributes[] = {0, 0};
		int     iResults[30];
		//int     nPFD = 0;
		int     pixelFormat;
		unsigned int numFormats;
		int		status;

		PIXELFORMATDESCRIPTOR temppfd = { 
			sizeof(PIXELFORMATDESCRIPTOR),   // size of this pfd 
			1,                     // version number 
			PFD_DRAW_TO_WINDOW |   // support window 
			PFD_GENERIC_ACCELERATED | // accelerated
			PFD_SUPPORT_OPENGL |   // support OpenGL 
			PFD_DOUBLEBUFFER,      // double buffered 
			PFD_TYPE_RGBA,         // RGBA type 
			(byte)Q_ftol(gl_colorbits->value),// desktop color depth 
			0, 0, 0, 0, 0, 0,      // color bits ignored 
			(byte)Q_ftol(gl_alphabits->value), // alpha buffer 
			0,                     // shift bit ignored 
			0,                     // no accumulation buffer 
			0, 0, 0, 0,            // accum bits ignored 
			(byte)Q_ftol(gl_depthbits->value), // z-buffer 
			(byte)Q_ftol(gl_stencilbits->value), // no stencil buffer 
			0,                     // no auxiliary buffer 
			PFD_MAIN_PLANE,        // main layer 
			0,                     // reserved 
			0, 0, 0                // layer masks ignored 
		};
		HGLRC hGLRC;

		HWND temphwnd = CreateWindowEx(0L,OPENGL_CLASS,"R1GL OpenGL PFD Detection Window",WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,0,0,1,1,glw_state.hWnd,0,glw_state.hInstance,NULL);

		HDC hDC = GetDC (temphwnd);

		// Set up OpenGL
		pixelFormat = ChoosePixelFormat(hDC, &temppfd);

		if (!pixelFormat)
		{
			ri.Con_Printf (PRINT_ALL, "�����������̨�������ChoosePixelFormat (%dc/%dd/%da/%ds) failed. Error %d.\n", (int)gl_colorbits->value, (int)gl_depthbits->value, (int)gl_alphabits->value, (int)gl_stencilbits->value, GetLastError());
			ReleaseDC (temphwnd, hDC);
			DestroyWindow (temphwnd);
			temphwnd = NULL;
			return init_regular();
		}

		if (SetPixelFormat(hDC, pixelFormat, &temppfd) == FALSE)
		{
			ri.Con_Printf (PRINT_ALL, "�����������̨������� SetPixelFormat (%d) failed. Error %d.\n", pixelFormat, GetLastError());
			ReleaseDC (temphwnd, hDC);
			DestroyWindow (temphwnd);
			temphwnd = NULL;
			return init_regular();
		}

		// Create a rendering context
		hGLRC = qwglCreateContext(hDC);
		if (!hGLRC) {
			ReleaseDC (temphwnd, hDC);
			DestroyWindow (temphwnd);
			temphwnd = NULL;
			ri.Con_Printf (PRINT_ALL, "�����������̨������� qwglCreateContext failed\n");
			return init_regular();
		}

		// Make the rendering context current
		if (!(qwglMakeCurrent(hDC, hGLRC))) {
			ReleaseDC (temphwnd, hDC);
			DestroyWindow (temphwnd);
			temphwnd = NULL;
			ri.Con_Printf (PRINT_ALL, "�����������̨������� qwglMakeCurrent failed\n");
			return init_regular();
		}

		{
			const char *s;
			s = qglGetString( GL_RENDERER );

			if (strcmp (s, "GDI Generic") == 0) {
				ri.Con_Printf (PRINT_ALL, "�����������̨������� no hardware accelerated pixelformats matching your current settings (try editing gl_colorbits/gl_alphabits/gl_depthbits/gl_stencilbits)\n");
				return init_regular();
			}
			ri.Cvar_Get ("vid_renderer", s, CVAR_NOSET);
			ri.Con_Printf (PRINT_ALL, "Getting capabilities of '%s'\n", s);
		}

		wglGetExtensionsStringARB = (PFNWGLGETEXTENSIONSSTRINGARBPROC)qwglGetProcAddress("wglGetExtensionsStringARB");
		wglGetExtensionsStringEXT = (PFNWGLGETEXTENSIONSSTRINGEXTPROC)qwglGetProcAddress("wglGetExtensionsStringEXT");

		if (!wglGetExtensionsStringARB || !wglGetExtensionsStringARB)
		{
			ReleaseDC (temphwnd, hDC);
			DestroyWindow (temphwnd);
			temphwnd = NULL;
			ri.Con_Printf (PRINT_ALL, "GLimp_InitGL() - qwglGetProcAddress (wglGetExtensionsString) failed, falling back to regular PFD\n");
			if (!init_regular()) {
				ri.Con_Printf (PRINT_ALL, "�����������̨������� init_regular () failed\n");
				return false;
			} else {
				return true;
			}
		}

		if (!(init_extensions ())) {
			ReleaseDC (temphwnd, hDC);
			DestroyWindow (temphwnd);
			temphwnd = NULL;
			ri.Con_Printf (PRINT_ALL, "GLimp_InitGL() - init_extensions () failed, falling back to regular PFD\n");
			if (!init_regular()) {
				ri.Con_Printf (PRINT_ALL, "�����������̨������� init_regular () failed\n");
				return false;
			} else {
				return true;
			}
		}

		// make no rendering context current
		qwglMakeCurrent(NULL, NULL);

		// Destroy the rendering context...
		qwglDeleteContext(hGLRC);
		hGLRC = NULL;

		// Get the number of pixel format available
		iAttributes[0] = WGL_NUMBER_PIXEL_FORMATS_ARB;

		if (wglGetPixelFormatAttribivARB(hDC, 0, 0, 1, iAttributes, iResults) == GL_FALSE) {
			ReleaseDC (temphwnd, hDC);
			DestroyWindow (temphwnd);
			temphwnd = NULL;
			ri.Con_Printf (PRINT_ALL, "GLimp_InitGL() - wglGetPixelFormatAttribivARB failed, falling back to regular PFD\n");
			if (!init_regular()) {
				ri.Con_Printf (PRINT_ALL, "�����������̨������� init_regular () failed\n");
				return false;
			} else {
				return true;
			}
		}

		//nPFD = iResults[0];

		// Choose a Pixel Format Descriptor (PFD) with multisampling support.
		iAttributes[0] = WGL_DOUBLE_BUFFER_ARB;
		iAttributes[1] = TRUE;

		iAttributes[2] = WGL_COLOR_BITS_ARB;
		iAttributes[3] = (int)gl_colorbits->value;

		iAttributes[4] = WGL_DEPTH_BITS_ARB;
		iAttributes[5] = (int)gl_depthbits->value;

		iAttributes[6] = WGL_ALPHA_BITS_ARB;
		iAttributes[7] = (int)gl_alphabits->value;

		iAttributes[8] = WGL_STENCIL_BITS_ARB;
		iAttributes[9] = (int)gl_stencilbits->value;

		iAttributes[10] = _is_multisample ? WGL_SAMPLE_BUFFERS_ARB : 0;
		iAttributes[11] = _is_multisample ? TRUE : 0;

		iAttributes[12] = _is_multisample ? WGL_SAMPLES_ARB : 0;
		iAttributes[13] = _is_multisample ? (int)gl_ext_samples->value : 0;

		iAttributes[14] = 0;
		iAttributes[15] = 0;

		// First attempt...
		status = wglChoosePixelFormatARB(hDC, iAttributes, fAttributes, 1, &pixelFormat, &numFormats);
		// Failure happens not only when the function fails, but also when no matching pixel format has been found
		if (status == FALSE || numFormats == 0)
		{
			ReleaseDC (temphwnd, hDC);
			DestroyWindow (temphwnd);
			temphwnd = NULL;
			ri.Con_Printf (PRINT_ALL, "GLimp_InitGL() - wglChoosePixelFormatARB failed, falling back to regular PFD\n");
			if (!init_regular()) {
				ri.Con_Printf (PRINT_ALL, "�����������̨������� init_regular () failed\n");
				return false;
			} else {
				return true;
			}
		}
		else
		{
			// Fill the list of attributes we are interested in
			iAttributes[0 ] = WGL_PIXEL_TYPE_ARB;
			iAttributes[1 ] = WGL_COLOR_BITS_ARB;
			iAttributes[2 ] = WGL_RED_BITS_ARB;
			iAttributes[3 ] = WGL_GREEN_BITS_ARB;
			iAttributes[4 ] = WGL_BLUE_BITS_ARB;
			iAttributes[5 ] = WGL_ALPHA_BITS_ARB;
			iAttributes[6 ] = WGL_DEPTH_BITS_ARB;
			iAttributes[7 ] = WGL_STENCIL_BITS_ARB;

			// Since WGL_ARB_multisample and WGL_pbuffer are extensions, we must check if
			// those extensions are supported before passing the corresponding enums
			// to the driver. This could cause an error if they are not supported.
			iAttributes[8 ] = _is_multisample ? WGL_SAMPLE_BUFFERS_ARB : WGL_PIXEL_TYPE_ARB;
			iAttributes[9 ] = _is_multisample ? WGL_SAMPLES_ARB : WGL_PIXEL_TYPE_ARB;
			iAttributes[12] = WGL_PIXEL_TYPE_ARB;
			iAttributes[10] = WGL_DRAW_TO_WINDOW_ARB;
			iAttributes[11] = WGL_DRAW_TO_BITMAP_ARB;
			iAttributes[13] = WGL_DOUBLE_BUFFER_ARB;
			iAttributes[14] = WGL_STEREO_ARB;
			iAttributes[15] = WGL_ACCELERATION_ARB;
			iAttributes[16] = WGL_NEED_PALETTE_ARB;
			iAttributes[17] = WGL_NEED_SYSTEM_PALETTE_ARB;
			iAttributes[18] = WGL_SWAP_LAYER_BUFFERS_ARB;
			iAttributes[19] = WGL_SWAP_METHOD_ARB;
			iAttributes[20] = WGL_NUMBER_OVERLAYS_ARB;
			iAttributes[21] = WGL_NUMBER_UNDERLAYS_ARB;
			iAttributes[22] = WGL_TRANSPARENT_ARB;
			iAttributes[23] = WGL_SUPPORT_GDI_ARB;
			iAttributes[24] = WGL_SUPPORT_OPENGL_ARB;

			if (wglGetPixelFormatAttribivARB(hDC, pixelFormat, 0, 25, iAttributes, iResults) == GL_FALSE) {
				ri.Con_Printf (PRINT_ALL, "�����������̨������� wglGetPixelFormatAttribivARB failed\n");
				ReleaseDC (temphwnd, hDC);
				DestroyWindow (temphwnd);
				temphwnd = NULL;
				return init_regular();
			}

			ri.Con_Printf ( PRINT_ALL ,"R1GL PFD: %d matching formats, chose %d\n  color(%d-bits (red:%d, green:%d, blue:%d)), z(%d-bits), alpha(%d-bits), stencil(%d-bits)\n",
				numFormats, pixelFormat, iResults[1], iResults[2], iResults[3], iResults[4], iResults[6], iResults[5], iResults[7]);

			if (_is_multisample) {
				qglEnable(GL_MULTISAMPLE_ARB);
				if (gl_config.r1gl_GL_EXT_nv_multisample_filter_hint) {
					if (!strcmp (gl_ext_nv_multisample_filter_hint->string, "nicest"))
						qglHint(GL_MULTISAMPLE_FILTER_HINT_NV, GL_NICEST);
					else
						qglHint(GL_MULTISAMPLE_FILTER_HINT_NV, GL_FASTEST);
				}

				if (iResults[8])
					ri.Con_Printf ( PRINT_ALL ,"  using multisampling (FSAA), %d samples per pixel\n", iResults[9]);
				else
					ri.Con_Printf ( PRINT_ALL ,"  multisampling (FSAA) setup FAILED.\n");
			}

			if (iResults[15] != WGL_FULL_ACCELERATION_ARB) {
				ri.Con_Printf ( PRINT_ALL, "********** WARNING **********\npixelformat %d is NOT hardware accelerated!\n*****************************\n", pixelFormat);
			}

			ReleaseDC (temphwnd, hDC);
			DestroyWindow (temphwnd);
			temphwnd = NULL;

			//glw_state.hDC = GetDC (glw_state.hWnd);
			if ( ( glw_state.hDC = GetDC( glw_state.hWnd ) ) == NULL )
			{
				ri.Con_Printf( PRINT_ALL, "�����������̨������� GetDC failed\n" );
				return false;
			}

			SetPixelFormat (glw_state.hDC, pixelFormat, &temppfd);

			/*
			** startup the OpenGL subsystem by creating a context and making
			** it current
			*/
			if ( ( glw_state.hGLRC = qwglCreateContext( glw_state.hDC ) ) == 0 )
			{
				ri.Con_Printf (PRINT_ALL, "�����������̨������� qwglCreateContext failed (%d)\n", GetLastError());
				goto fail;
			}

			if ( !qwglMakeCurrent( glw_state.hDC, glw_state.hGLRC ) )
			{
				ri.Con_Printf (PRINT_ALL, "�����������̨������� qwglMakeCurrent failed\n");
				goto fail;
			}

			gl_config.bitDepth = iResults[1];		
			gl_config.wglPFD = true;
		}
	/*} else {

		if ( !VerifyDriver() )
		{
			ri.Con_Printf( PRINT_ALL, "�����������̨������� no hardware acceleration detected\n" );
			goto fail;
		}

		ri.Con_Printf( PRINT_ALL, "GL PFD: pf(%d) color(%d-bits) Z(%d-bit)\n", pixelformat, ( int ) pfd.cColorBits, ( int ) pfd.cDepthBits );
		gl_config.bitDepth = pfd.cColorBits;*/
	}

	

	










	return true;

fail:
	if ( glw_state.hGLRC )
	{
		qwglDeleteContext( glw_state.hGLRC );
		glw_state.hGLRC = NULL;
	}

	if ( glw_state.hDC )
	{
		ReleaseDC( glw_state.hWnd, glw_state.hDC );
		glw_state.hDC = NULL;
	}
	return false;
}

/*
** GLimp_BeginFrame
*/
#ifdef STEREO_SUPPORT
void GLimp_BeginFrame( float camera_separation )
#else
void GLimp_BeginFrame( void )
#endif
{
#if 0
	if ( gl_bitdepth->modified )
	{
		if ( FLOAT_NE_ZERO(gl_bitdepth->value) && !glw_state.allowdisplaydepthchange )
		{
			ri.Cvar_SetValue( "gl_bitdepth", 0 );
			ri.Con_Printf( PRINT_ALL, "gl_bitdepth requires Win95 OSR2.x or WinNT 4.x\n" );
		}
		gl_bitdepth->modified = false;
	}
#endif

#ifdef STEREO_SUPPORT
	if ( camera_separation < 0 && gl_state.stereo_enabled )
	{
		qglDrawBuffer( GL_BACK_LEFT );
	}
	else if (FLOAT_GT_ZERO (camera_separation) && gl_state.stereo_enabled )
	{
		qglDrawBuffer( GL_BACK_RIGHT );
	}
	else
#endif
	{
		qglDrawBuffer( GL_BACK );
	}
}

/*
** GLimp_EndFrame
** 
** Responsible for doing a swapbuffers and possibly for other stuff
** as yet to be determined.  Probably better not to make this a GLimp
** function and instead do a call to GLimp_SwapBuffers.
*/

void Draw_AddText (void);
void EXPORT GLimp_EndFrame (void)
{
	static int iDrawBuffer = 0;

	if (defer_drawing)
		Draw_AddText();

	if (gl_drawbuffer->modified)
	{
		gl_drawbuffer->modified = false;
		iDrawBuffer = stricmp( gl_drawbuffer->string, "GL_BACK" );
	}

	if (gl_defertext->modified)
	{
		gl_defertext->modified = false;
		defer_drawing = (int)gl_defertext->value;
	}

	if (iDrawBuffer == 0)
	{
		if (gl_config.wglPFD)
		{
			if ( !qwglSwapBuffers( glw_state.hDC ) )
				ri.Sys_Error( ERR_FATAL, "GLimp_EndFrame() - SwapBuffers() failed!\n" );
		}
		else
		{
			if ( !SwapBuffers( glw_state.hDC ) )
				ri.Sys_Error( ERR_FATAL, "GLimp_EndFrame() - SwapBuffers() failed!\n" );
		}
	}
}

void RestoreDesktopSettings (void)
{
	if (ChangeDisplaySettings( &originalDesktopMode, 0 ) != DISP_CHANGE_SUCCESSFUL )
	{
		ri.Sys_Error (ERR_FATAL, "Couldn't restore desktop display settings");
	}
}

void RestoreQ2Settings (void)
{
	if (ChangeDisplaySettings( &fullScreenMode, CDS_FULLSCREEN ) != DISP_CHANGE_SUCCESSFUL )
	{
		ri.Sys_Error (ERR_FATAL, "Couldn't restore Quake 2 display settings");
	}
}

/*
** GLimp_AppActivate
*/
qboolean R_SetMode (void);
void EXPORT GLimp_AppActivate( qboolean active )
{
	if ( active )
	{
		if (IsIconic (glw_state.hWnd))
			return;

		if ( FLOAT_NE_ZERO(vid_fullscreen->value) && usingDesktopSettings )
		{
			RestoreQ2Settings ();
			usingDesktopSettings = false;
		}

		SetForegroundWindow( glw_state.hWnd );
		ShowWindow( glw_state.hWnd, SW_RESTORE );
	}
	else
	{
		if ( FLOAT_NE_ZERO(vid_fullscreen->value) && FLOAT_NE_ZERO (vid_restore_on_switch->value) )
		{
			ShowWindow( glw_state.hWnd, SW_MINIMIZE );
			RestoreDesktopSettings();
			usingDesktopSettings = true;
		}
	}
}
