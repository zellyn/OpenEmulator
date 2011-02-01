
/**
 * libemulation-hal
 * OpenGL HAL
 * (C) 2010-2011 by Marc S. Ressl (mressl@umich.edu)
 * Released under the GPL
 *
 * Implements an OpenGL HAL.
 */

#include <math.h>

#include "OpenGLHAL.h"

#include "OpenGLHALSignalProcessing.h"

#define DEFAULT_WIDTH	640
#define DEFAULT_HEIGHT	480

OpenGLHAL::OpenGLHAL()
{
	setCapture = NULL;
	setKeyboardFlags = NULL;
	
	configuration.zoomMode = CANVAS_ZOOMMODE_FIT_CANVAS;
	configuration.processMode = CANVAS_PROCESSMODE_COMPOSITE;
	configuration.captureMode = CANVAS_CAPTUREMODE_NO_CAPTURE;
	configuration.defaultViewSize = OEMakeSize(DEFAULT_WIDTH, DEFAULT_HEIGHT);
	configuration.canvasSize = OEMakeSize(DEFAULT_WIDTH, DEFAULT_HEIGHT);
	configuration.contentRect = OEMakeRect(0, 0, 1, 1);
	updateConfiguration = true;
	
	nextFrame = NULL;
	frame = NULL;
	
	capture = OPENGLHAL_CAPTURE_NONE;
	
	for (int i = 0; i < CANVAS_KEYBOARD_KEY_NUM; i++)
		keyDown[i] = false;
	keyDownCount = 0;
	ctrlAltWasPressed = false;
	mouseEntered = false;
	for (int i = 0; i < CANVAS_MOUSE_BUTTON_NUM; i++)
		mouseButtonDown[i] = false;
	for (int i = 0; i < CANVAS_JOYSTICK_NUM; i++)
		for (int j = 0; j < CANVAS_JOYSTICK_BUTTON_NUM; j++)
			joystickButtonDown[i][j] = false;
}

// Video

void OpenGLHAL::open(CanvasSetCapture setCapture,
					 CanvasSetKeyboardFlags setKeyboardFlags,
					 void *userData)
{
	this->setCapture = setCapture;
	this->setKeyboardFlags = setKeyboardFlags;
	this->userData = userData;
	
	initOpenGL();
	
	pthread_mutex_init(&drawMutex, NULL);
}

void OpenGLHAL::close()
{
	pthread_mutex_destroy(&drawMutex);
	
	freeOpenGL();
	
	delete nextFrame;
	delete frame;
}

OESize OpenGLHAL::getDefaultViewSize()
{
	return configuration.defaultViewSize;
}

bool OpenGLHAL::update(float width, float height, float offset, bool draw)
{
	bool isShaderUpdated = false;
	bool isTextureUpdated = false;
	
	pthread_mutex_lock(&drawMutex);
	if (updateConfiguration)
	{
		frameConfiguration = configuration;
		updateConfiguration = false;
		
		isShaderUpdated = true;
		draw = true;
	}
	if (nextFrame)
	{
		if (frame)
			delete frame;
		
		frame = nextFrame;
		nextFrame = NULL;
		
		isTextureUpdated = true;
		draw = true;
	}
	pthread_mutex_unlock(&drawMutex);
	
	if (isShaderUpdated)
		updateShader();
	
	if (isTextureUpdated)
		updateTexture();
	
	if (draw)
	{
		updateFrame(height);
		drawCanvas(width, height);
	}
	
	return draw;
}

// OpenGL

bool OpenGLHAL::initOpenGL()
{
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	
	glEnable(GL_TEXTURE_2D);
	
	glGenTextures(OPENGLHAL_TEXTURE_END, glTextures);
	
	glTextureSize = OEMakeSize(0, 0);
	glProgram = 0;
	
	loadShader();
	
	return true;
}

bool OpenGLHAL::loadShader()
{
	const GLchar *source = "\
	uniform sampler2D texture;\
	uniform vec2 texture_size;\
	uniform float barrel;\
	uniform vec2 barrel_center;\
	uniform float comp_black;\
	uniform float comp_fsc;\
	uniform vec3 c0;\
	uniform vec3 c1;\
	uniform vec3 c2;\
	uniform vec3 c3;\
	uniform vec3 c4;\
	uniform vec3 c5;\
	uniform vec3 c6;\
	uniform vec3 c7;\
	uniform vec3 c8;\
	uniform vec3 decoderRow1;\
	uniform vec3 decoderRow2;\
	uniform vec3 decoderRow3;\
	uniform float scanline;\
	uniform float brightness;\
	\
	float PI = 3.14159265358979323846264;\
	\
	vec3 demodulate(vec2 q)\
	{\
	vec3 p = texture2D(texture, q).xyz - comp_black;\
	float phase = 2.0 * PI * comp_fsc * texture_size.x * q.x;\
	p.y *= sin(phase);\
	p.z *= cos(phase);\
	return p;\
	}\
	\
	vec3 demodulate2(vec2 q, float i)\
	{\
	return demodulate(vec2(q.x + i, q.y)) + demodulate(vec2(q.x - i, q.y));\
	}\
	\
	void main(void)\
	{\
	vec2 q = gl_TexCoord[0].xy;\
	q -= barrel_center;\
	q += barrel * q * (q.x * q.x + q.y * q.y);\
	q += barrel_center;\
	\
	vec3 p = demodulate(q) * c0;\
	p += demodulate2(q, 1.0 / texture_size.x) * c1;\
	p += demodulate2(q, 2.0 / texture_size.x) * c2;\
	p += demodulate2(q, 3.0 / texture_size.x) * c3;\
	p += demodulate2(q, 4.0 / texture_size.x) * c4;\
	p += demodulate2(q, 5.0 / texture_size.x) * c5;\
	p += demodulate2(q, 6.0 / texture_size.x) * c6;\
	p += demodulate2(q, 7.0 / texture_size.x) * c7;\
	p += demodulate2(q, 8.0 / texture_size.x) * c8;\
	p = mat3(decoderRow1, decoderRow2, decoderRow3) * p;\
	float s = sin(PI * q.y * texture_size.y);\
	p *= (1.0 - scanline) + scanline * s * s;\
	p += brightness;\
	gl_FragColor = vec4(p, 1.0);\
	}\
	";
	GLint sourceLength = strlen(source);
	
	glFragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
	glShaderSource(glFragmentShader, 1, &source, &sourceLength);
	glCompileShader(glFragmentShader);
	
	GLint isCompiled;
	glGetShaderiv(glFragmentShader, GL_COMPILE_STATUS, &isCompiled);
	
	if (!isCompiled)
	{
		string errorString = "could not compile OpenGL fragment shader";
		
		GLint bufSize = 0;
		glGetShaderiv(glFragmentShader, GL_INFO_LOG_LENGTH, &bufSize);
		
		if (bufSize > 0)
		{
			GLsizei infoLogLength = 0;
			
			vector<char> infoLog;
			infoLog.resize(bufSize);
			
			glGetShaderInfoLog(glFragmentShader, bufSize,
							   &infoLogLength, &infoLog.front());
			
			errorString += "\n";
			errorString += &infoLog.front();
		}
		
		log(errorString);
		
		return false;
	}
	
	glProgram = glCreateProgram();
	glAttachShader(glProgram, glFragmentShader);
	glLinkProgram(glProgram);
	
	GLint isLinked;
	glGetShaderiv(glProgram, GL_LINK_STATUS, &isLinked);
	
	if (!isLinked)
	{
		string errorString = "could not link OpenGL program";
		
		GLint bufSize = 0;
		glGetProgramiv(glProgram, GL_INFO_LOG_LENGTH, &bufSize);
		
		if (bufSize > 0)
		{
			GLsizei infoLogLength = 0;
			
			vector<char> infoLog;
			infoLog.resize(bufSize);
			
			glGetShaderInfoLog(glFragmentShader, bufSize,
							   &infoLogLength, &infoLog.front());
			
			errorString += "\n";
			errorString += &infoLog.front();
		}
		
		log(errorString);
		
		return false;
	}
	
	return true;
}

void OpenGLHAL::freeOpenGL()
{
	glDeleteTextures(OPENGLHAL_TEXTURE_END, glTextures);
}

bool OpenGLHAL::updateShader()
{
	if (!glProgram)
		return true;
	
	// Luminance and chrominance filters
	Vector w = Vector::chebyshevWindow(17, 50.0);
	w = w.normalize();
	
	Vector wy;
	wy = w * Vector::lanczosWindow(17, frameConfiguration.compositeLumaCutoffFrequency);
	wy = wy.normalize();
	
	Vector wc;
	wc = w * Vector::lanczosWindow(17, frameConfiguration.compositeChromaCutoffFrequency);
	wc = wc.normalize() * 2.0;
	
	// Decoder matrix
	Matrix3 m(1, 0, 0,
			  0, 1, 0,
			  0, 0, 1);
	// Contrast
	m *= frameConfiguration.contrast;
	// Y'UV or Y'IQ to RGB
	// Matrices from "Digital Video and HDTV Algorithms and Interfaces"
	switch (frameConfiguration.compositeDecoder)
	{
		case CANVAS_COMPOSITEDECODER_NTSC_YUV:
			m *= Matrix3(1, 1, 1,
						 0, -0.394642, 2.032062,
						 1.139883,-0.580622, 0);
			break;
		case CANVAS_COMPOSITEDECODER_NTSC_YIQ:
			m *= Matrix3(1, 1, 1,
						 0.955986, -0.272013, -1.106740,
						 0.620825, -0.647204, 1.704230);
			break;
		case CANVAS_COMPOSITEDECODER_CXA2025AS:
			m *= Matrix3(1, 1, 1,
						 1.630, -0.378, -1.089,
						 0.317, -0.466, 1.677);
			break;
	}
	// Hue
	float hue = 2 * M_PI * frameConfiguration.compositeHue;
	m *= Matrix3(1.0, 0.0, 0.0,
				 0.0, cosf(hue), -sinf(hue),
				 0.0, sinf(hue), cosf(hue));
	// Saturation
	m *= Matrix3(1.0, 0.0, 0.0,
				 0.0, frameConfiguration.saturation, 0.0,
				 0.0, 0.0, frameConfiguration.saturation);
	// Dynamic range gain
	float levelRange = (frameConfiguration.compositeWhiteLevel -
						frameConfiguration.compositeBlackLevel);
	if (fabs(levelRange) < 0.01)
		levelRange = 0.01;
	m *= 1.0 / levelRange;
	
	glUseProgram(glProgram);
	
	glUniform1f(glGetUniformLocation(glProgram, "barrel"),
				frameConfiguration.barrel);
	glUniform1f(glGetUniformLocation(glProgram, "comp_black"),
				frameConfiguration.compositeBlackLevel);
	glUniform1f(glGetUniformLocation(glProgram, "comp_fsc"),
				frameConfiguration.compositeCarrierFrequency);
	
	glUniform3f(glGetUniformLocation(glProgram, "c0"),
				wy.getValue(8), wc.getValue(8), wc.getValue(8));
	glUniform3f(glGetUniformLocation(glProgram, "c1"),
				wy.getValue(7), wc.getValue(7), wc.getValue(7));
	glUniform3f(glGetUniformLocation(glProgram, "c2"),
				wy.getValue(6), wc.getValue(6), wc.getValue(6));
	glUniform3f(glGetUniformLocation(glProgram, "c3"),
				wy.getValue(5), wc.getValue(5), wc.getValue(5));
	glUniform3f(glGetUniformLocation(glProgram, "c4"),
				wy.getValue(4), wc.getValue(4), wc.getValue(4));
	glUniform3f(glGetUniformLocation(glProgram, "c5"),
				wy.getValue(3), wc.getValue(3), wc.getValue(3));
	glUniform3f(glGetUniformLocation(glProgram, "c6"),
				wy.getValue(2), wc.getValue(2), wc.getValue(2));
	glUniform3f(glGetUniformLocation(glProgram, "c7"),
				wy.getValue(1), wc.getValue(1), wc.getValue(1));
	glUniform3f(glGetUniformLocation(glProgram, "c8"),
				wy.getValue(0), wc.getValue(0), wc.getValue(0));
	
	glUniform3f(glGetUniformLocation(glProgram, "decoderRow1"),
				m.getValue(0, 0), m.getValue(0, 1), m.getValue(0, 2));
	glUniform3f(glGetUniformLocation(glProgram, "decoderRow2"),
				m.getValue(1, 0), m.getValue(1, 1), m.getValue(1, 2));
	glUniform3f(glGetUniformLocation(glProgram, "decoderRow3"),
				m.getValue(2, 0), m.getValue(2, 1), m.getValue(2, 2));
	
	glUniform1f(glGetUniformLocation(glProgram, "brightness"),
				frameConfiguration.brightness);
	
	glUseProgram(0);
	
	return true;
}

bool OpenGLHAL::updateTexture()
{
	GLint format;
	
	if (frame->getFormat() == OEIMAGE_FORMAT_LUMINANCE)
		format = GL_LUMINANCE;
	else if (frame->getFormat() == OEIMAGE_FORMAT_RGB)
		format = GL_RGB;
	else if (frame->getFormat() == OEIMAGE_FORMAT_RGBA)
		format = GL_RGBA;
	
	glBindTexture(GL_TEXTURE_2D, glTextures[OPENGLHAL_TEXTURE_FRAME]);
	
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
	
	OESize newTextureSize = OEMakeSize(getNextPowerOf2(frame->getSize().width),
									   getNextPowerOf2(frame->getSize().height));
	
	if ((glTextureSize.width != newTextureSize.width) ||
		(glTextureSize.height != newTextureSize.height))
	{
		glTextureSize = newTextureSize;
		
		vector<char> dummy;
		dummy.resize(glTextureSize.width * glTextureSize.height);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
					 glTextureSize.width, glTextureSize.height, 0,
					 GL_LUMINANCE, GL_UNSIGNED_BYTE, &dummy.front());
	}
	
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
					frame->getSize().width, frame->getSize().height,
					format, GL_UNSIGNED_BYTE,
					frame->getPixels());
	
	if (glProgram)
	{
		glUseProgram(glProgram);
		
		glBindTexture(GL_TEXTURE_2D, glTextures[OPENGLHAL_TEXTURE_FRAME]);
		glUniform1i(glGetUniformLocation(glProgram, "texture"),
					0);
		
		glUniform2f(glGetUniformLocation(glProgram, "texture_size"),
					glTextureSize.width, glTextureSize.height);
		
		glUseProgram(0);
	}
	
	return true;
}

void OpenGLHAL::updateFrame(float height)
{
	if (!glProgram)
		return;
	
	glUseProgram(glProgram);
	
	OEPoint barrelCenter;
	barrelCenter.x = ((0.5 - frameConfiguration.contentRect.origin.x) /
					  frameConfiguration.contentRect.size.width *
					  frame->getSize().width / glTextureSize.width);
	barrelCenter.y = ((0.5 - frameConfiguration.contentRect.origin.y) /
					  frameConfiguration.contentRect.size.height *
					  frame->getSize().height / glTextureSize.height);
	glUniform2f(glGetUniformLocation(glProgram, "barrel_center"),
				barrelCenter.x,
				barrelCenter.y);
	
	float scanlineHeight = (height / frame->getSize().height *
							frameConfiguration.contentRect.size.height);
	float alpha = frameConfiguration.compositeScanlineAlpha;
	float scanline = ((scanlineHeight > 2.0) ? alpha :
					  (scanlineHeight < 1.5) ? 0 :
					  (scanlineHeight - 1.5) * 2 * alpha);
	glUniform1f(glGetUniformLocation(glProgram, "scanline"),
				scanline);
	
	glUseProgram(0);
}

bool OpenGLHAL::drawCanvas(float width, float height)
{
	glViewport(0, 0, width, height);
	
	float clearColor = ((frameConfiguration.processMode == CANVAS_PROCESSMODE_COMPOSITE) ?
						frameConfiguration.brightness : 0);
	glClearColor(clearColor, clearColor, clearColor, 0);
	glClear(GL_COLOR_BUFFER_BIT);
	
	if (!frame)
		return false;
	
	OESize frameSize = frame->getSize();
	OERect textureFrame = OEMakeRect(0, 0,
									 frameSize.width / glTextureSize.width, 
									 frameSize.height / glTextureSize.height);
	
	float viewAspectRatio = width / height;
	OERect viewFrame = OEMakeRect(-1, -1, 2, 2);
	
	float canvasAspectRatio = (frameConfiguration.canvasSize.width /
							   frameConfiguration.canvasSize.height);
	
	// Apply view mode
	if (frameConfiguration.zoomMode == CANVAS_ZOOMMODE_FIT_WIDTH)
	{
		float ratio = viewAspectRatio / canvasAspectRatio;
		viewFrame.size.height = 2 * ratio;
	}
	else
	{
		if (canvasAspectRatio > viewAspectRatio)
		{
			float ratio = viewAspectRatio / canvasAspectRatio;
			viewFrame.origin.y = -ratio;
			viewFrame.size.height = 2 * ratio;
		}
		else
		{
			float ratio = canvasAspectRatio / viewAspectRatio;
			viewFrame.origin.x = -ratio;
			viewFrame.size.width = 2 * ratio;
		}
	}
	
	// Apply content rect
	viewFrame.origin.x += frameConfiguration.contentRect.origin.x * 2.0;
	viewFrame.origin.y += (1.0 - frameConfiguration.contentRect.size.height - 
						   frameConfiguration.contentRect.origin.y) * 2.0;
	viewFrame.size.width *= frameConfiguration.contentRect.size.width;
	viewFrame.size.height *= frameConfiguration.contentRect.size.height;
	
	// Bind texture
	glBindTexture(GL_TEXTURE_2D, glTextures[OPENGLHAL_TEXTURE_FRAME]);
	
	int viewFrameWidth = width * viewFrame.size.width / 2.0;
	GLint param = GL_LINEAR;
	if ((viewFrameWidth == (int) frameSize.width) &&
		(viewFrame.origin.x == -1) && (viewFrame.origin.y == -1))
		param = GL_NEAREST;
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, param);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, param);
	
	// Draw
	if (frameConfiguration.processMode == CANVAS_PROCESSMODE_COMPOSITE)
		glUseProgram(glProgram);
	
	glBegin(GL_QUADS);
	glTexCoord2f(OEMinX(textureFrame), OEMinY(textureFrame));
	glVertex2f(OEMinX(viewFrame), OEMaxY(viewFrame));
	glTexCoord2f(OEMaxX(textureFrame), OEMinY(textureFrame));
	glVertex2f(OEMaxX(viewFrame), OEMaxY(viewFrame));
	glTexCoord2f(OEMaxX(textureFrame), OEMaxY(textureFrame));
	glVertex2f(OEMaxX(viewFrame), OEMinY(viewFrame));
	glTexCoord2f(OEMinX(textureFrame), OEMaxY(textureFrame));
	glVertex2f(OEMinX(viewFrame), OEMinY(viewFrame));
	glEnd();
	
	glUseProgram(0);
	
	return true;
}

// HID

void OpenGLHAL::updateCapture(OpenGLHALCapture capture)
{
	//	log("updateCapture");
	
	if (this->capture == capture)
		return;
	this->capture = capture;
	
	if (setCapture)
		setCapture(userData, capture);
}

void OpenGLHAL::becomeKeyWindow()
{
}

void OpenGLHAL::resignKeyWindow()
{
	resetKeysAndButtons();
	
	ctrlAltWasPressed = false;
	
	updateCapture(OPENGLHAL_CAPTURE_NONE);
}

void OpenGLHAL::sendSystemEvent(int usageId)
{
	postHIDNotification(CANVAS_SYSTEMKEYBOARD_DID_CHANGE, usageId, 0);
}

void OpenGLHAL::setKey(int usageId, bool value)
{
	if (keyDown[usageId] == value)
		return;
	
//	log("key " + getHexString(usageId) + ": " + getString(value));
//	log("keyDownCount " + getString(keyDownCount));
	
	keyDown[usageId] = value;
	keyDownCount += value ? 1 : -1;
	if ((keyDown[CANVAS_K_LEFTCONTROL] ||
		 keyDown[CANVAS_K_RIGHTCONTROL]) &&
		(keyDown[CANVAS_K_LEFTALT] ||
		 keyDown[CANVAS_K_RIGHTALT]))
		ctrlAltWasPressed = true;
	
	postHIDNotification(CANVAS_KEYBOARD_DID_CHANGE, usageId, value);
	
	if ((capture == OPENGLHAL_CAPTURE_KEYBOARD_AND_DISCONNECT_MOUSE_CURSOR) &&
		!keyDownCount && ctrlAltWasPressed)
	{
		ctrlAltWasPressed = false;
		
		updateCapture(OPENGLHAL_CAPTURE_NONE);
	}
}

void OpenGLHAL::postHIDNotification(int notification, int usageId, float value)
{
	CanvasHIDNotification data = {usageId, value};
	notify(this, notification, &data);
}

void OpenGLHAL::sendUnicodeKeyEvent(int unicode)
{
//	log("unicode " + getHexString(unicode));
	
	postHIDNotification(CANVAS_UNICODEKEYBOARD_DID_CHANGE, unicode, 0);
}

void OpenGLHAL::enterMouse()
{
	mouseEntered = true;
	
	if (configuration.captureMode == CANVAS_CAPTUREMODE_CAPTURE_ON_MOUSE_ENTER)
		updateCapture(OPENGLHAL_CAPTURE_KEYBOARD_AND_HIDE_MOUSE_CURSOR);
	
	postHIDNotification(CANVAS_POINTER_DID_CHANGE,
						CANVAS_P_PROXIMITY,
						1.0);
}

void OpenGLHAL::exitMouse()
{
	mouseEntered = false;
	
	if (configuration.captureMode == CANVAS_CAPTUREMODE_CAPTURE_ON_MOUSE_ENTER)
		updateCapture(OPENGLHAL_CAPTURE_NONE);
	
	postHIDNotification(CANVAS_POINTER_DID_CHANGE,
						CANVAS_P_PROXIMITY,
						0.0);
}

void OpenGLHAL::setMousePosition(float x, float y)
{
	if (capture != OPENGLHAL_CAPTURE_KEYBOARD_AND_DISCONNECT_MOUSE_CURSOR)
	{
		postHIDNotification(CANVAS_POINTER_DID_CHANGE,
							CANVAS_P_X,
							x);
		postHIDNotification(CANVAS_POINTER_DID_CHANGE,
							CANVAS_P_Y,
							y);
	}
}

void OpenGLHAL::moveMouse(float rx, float ry)
{
	if (capture == OPENGLHAL_CAPTURE_KEYBOARD_AND_DISCONNECT_MOUSE_CURSOR)
	{
		postHIDNotification(CANVAS_MOUSE_DID_CHANGE,
							CANVAS_M_RELX,
							rx);
		postHIDNotification(CANVAS_MOUSE_DID_CHANGE,
							CANVAS_M_RELY,
							ry);
	}
}

void OpenGLHAL::setMouseButton(int index, bool value)
{
	if (index >= CANVAS_MOUSE_BUTTON_NUM)
		return;
	if (mouseButtonDown[index] == value)
		return;
	mouseButtonDown[index] = value;
	
	if (capture == OPENGLHAL_CAPTURE_KEYBOARD_AND_DISCONNECT_MOUSE_CURSOR)
		postHIDNotification(CANVAS_MOUSE_DID_CHANGE,
							CANVAS_M_BUTTON1 + index,
							value);
	else if ((configuration.captureMode == CANVAS_CAPTUREMODE_CAPTURE_ON_MOUSE_CLICK) &&
			 (capture == OPENGLHAL_CAPTURE_NONE) &&
			 (index == 0))
		updateCapture(OPENGLHAL_CAPTURE_KEYBOARD_AND_DISCONNECT_MOUSE_CURSOR);
	else
		postHIDNotification(CANVAS_POINTER_DID_CHANGE,
							CANVAS_P_BUTTON1 + index,
							value);
}

void OpenGLHAL::sendMouseWheelEvent(int index, float value)
{
	if (capture == OPENGLHAL_CAPTURE_KEYBOARD_AND_DISCONNECT_MOUSE_CURSOR)
		postHIDNotification(CANVAS_MOUSE_DID_CHANGE,
							CANVAS_M_WHEELX + index,
							value);
	else
		postHIDNotification(CANVAS_POINTER_DID_CHANGE,
							CANVAS_P_WHEELX + index,
							value);
}

void OpenGLHAL::setJoystickButton(int deviceIndex, int index, bool value)
{
	if (deviceIndex >= CANVAS_JOYSTICK_NUM)
		return;
	if (index >= CANVAS_JOYSTICK_BUTTON_NUM)
		return;
	if (joystickButtonDown[deviceIndex][index] == value)
		return;
	joystickButtonDown[deviceIndex][index] = value;
	
	postHIDNotification(CANVAS_JOYSTICK1_DID_CHANGE + deviceIndex,
						CANVAS_J_BUTTON1 + index,
						value);
}

void OpenGLHAL::setJoystickPosition(int deviceIndex, int index, float value)
{
	if (deviceIndex >= CANVAS_JOYSTICK_NUM)
		return;
	if (index >= CANVAS_JOYSTICK_AXIS_NUM)
		return;
	
	postHIDNotification(CANVAS_JOYSTICK1_DID_CHANGE + deviceIndex,
						CANVAS_J_AXIS1 + index,
						value);
}

void OpenGLHAL::sendJoystickHatEvent(int deviceIndex, int index, float value)
{
	if (deviceIndex >= CANVAS_JOYSTICK_NUM)
		return;
	if (index >= CANVAS_JOYSTICK_HAT_NUM)
		return;
	
	postHIDNotification(CANVAS_JOYSTICK1_DID_CHANGE + deviceIndex,
						CANVAS_J_AXIS1 + index,
						value);
}

void OpenGLHAL::moveJoystickBall(int deviceIndex, int index, float value)
{
	if (deviceIndex >= CANVAS_JOYSTICK_NUM)
		return;
	if (index >= CANVAS_JOYSTICK_RAXIS_NUM)
		return;
	
	postHIDNotification(CANVAS_JOYSTICK1_DID_CHANGE + deviceIndex,
						CANVAS_J_AXIS1 + index,
						value);
}

void OpenGLHAL::resetKeysAndButtons()
{
	for (int i = 0; i < CANVAS_KEYBOARD_KEY_NUM; i++)
		setKey(i, false);
	
	for (int i = 0; i < CANVAS_MOUSE_BUTTON_NUM; i++)
		setMouseButton(i, false);
	
	for (int i = 0; i < CANVAS_JOYSTICK_NUM; i++)
		for (int j = 0; j < CANVAS_JOYSTICK_BUTTON_NUM; j++)
			setJoystickButton(i, j, false);
}

bool OpenGLHAL::copy(string& value)
{
	return OEComponent::postMessage(this, CANVAS_COPY, &value);
}

bool OpenGLHAL::paste(string value)
{
	return OEComponent::postMessage(this, CANVAS_PASTE, &value);
}

bool OpenGLHAL::setConfiguration(CanvasConfiguration *configuration)
{
	if (!configuration)
		return false;
	
	if (this->configuration.captureMode != configuration->captureMode)
	{
		switch (configuration->captureMode)
		{
			case CANVAS_CAPTUREMODE_NO_CAPTURE:
				updateCapture(OPENGLHAL_CAPTURE_NONE);
				break;
				
			case CANVAS_CAPTUREMODE_CAPTURE_ON_MOUSE_CLICK:
				updateCapture(OPENGLHAL_CAPTURE_NONE);
				break;
				
			case CANVAS_CAPTUREMODE_CAPTURE_ON_MOUSE_ENTER:
				updateCapture(mouseEntered ? 
							  OPENGLHAL_CAPTURE_KEYBOARD_AND_HIDE_MOUSE_CURSOR : 
							  OPENGLHAL_CAPTURE_NONE);
				break;
		}
	}
	
	pthread_mutex_lock(&drawMutex);
	this->configuration = *configuration;
	updateConfiguration = true;
	pthread_mutex_unlock(&drawMutex);
	
	return true;
}

bool OpenGLHAL::postFrame(OEImage *frame)
{
	if (!frame)
		return false;
	
	pthread_mutex_lock(&drawMutex);
	if (nextFrame)
		delete nextFrame;
	
	nextFrame = new OEImage(*frame);
	pthread_mutex_unlock(&drawMutex);
	
	return true;
}

bool OpenGLHAL::postMessage(OEComponent *sender, int message, void *data)
{
	switch (message)
	{
		case CANVAS_CONFIGURE:
			return setConfiguration((CanvasConfiguration *)data);
			
		case CANVAS_POST_FRAME:
			return postFrame((OEImage *)data);
	}
	
	return OEComponent::postMessage(sender, message, data);
}