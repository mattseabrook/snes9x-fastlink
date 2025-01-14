/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#ifndef CGD3DCG_H
#define CGD3DCG_H

#include <vector>
#include <windows.h>
#include "CCGShader.h"
#include <vector>
#include <deque>
#include <array>

struct float2
{
	float x;
	float y;
};

class CD3DCG
{
private:
	typedef struct _parameterEntry {
		unsigned long rIndex;
		const char* semantic;
		bool isKnownParam;
		UINT streamNumber;
	} parameterEntry;
	typedef struct _shaderPass {
		cgScaleParams scaleParams;
		bool linearFilter;
        bool useFloatTex;
        unsigned int frameCounterMod;
		CGprogram cgVertexProgram, cgFragmentProgram;
		LPDIRECT3DTEXTURE9    tex;
		LPDIRECT3DVERTEXBUFFER9 vertexBuffer;
		LPDIRECT3DVERTEXDECLARATION9 vertexDeclaration;
		std::vector<parameterEntry> parameterMap;

		float2 outputSize;
		float2 textureSize;

		_shaderPass()  {cgVertexProgram=NULL;
					    cgFragmentProgram=NULL;
						tex=NULL;
						vertexBuffer=NULL;
						vertexDeclaration=NULL;}
	} shaderPass;
	typedef struct _prevPass {
		LPDIRECT3DTEXTURE9    tex;
		LPDIRECT3DVERTEXBUFFER9 vertexBuffer;
		float2 imageSize;
		float2 textureSize;
		_prevPass() {tex=NULL;
		             vertexBuffer=NULL;}
		_prevPass(const shaderPass &pass) {tex = pass.tex;
		                                   vertexBuffer = pass.vertexBuffer;
										   imageSize = pass.outputSize;
										   textureSize = pass.textureSize;}
	} prevPass;
	typedef struct _lookupTexture {
		char id[PATH_MAX];
		LPDIRECT3DTEXTURE9 tex;
		bool linearFilter;
		_lookupTexture() {tex=NULL;}
	} lookupTexture;

	std::vector<shaderPass> shaderPasses;
	std::vector<lookupTexture> lookupTextures;
	std::deque<prevPass> prevPasses;

	bool shaderLoaded;
	void checkForCgError(const char *situation);
	void setVertexStream(IDirect3DVertexBuffer9 *vertexBuffer, float2 inputSize, float2 textureSize, float2 outputSize);
	void setViewport(DWORD x, DWORD y, DWORD width, DWORD height);
	void setShaderVars(int pass);
	void ensureTextureSize(LPDIRECT3DTEXTURE9 &tex, float2 &texSize, float2 wantedSize,bool renderTarget, bool useFloat = false);
	void fillParameterMap(std::vector<parameterEntry> &map, CGparameter param);
	void setupVertexDeclaration(shaderPass &pass);
	void calculateMatrix();

	LPDIRECT3DDEVICE9     pDevice;
	CGcontext cgContext;
	unsigned int frameCnt;
	D3DMATRIX mvp;

public:
	CD3DCG(CGcontext cgContext,LPDIRECT3DDEVICE9 pDevice);
	~CD3DCG(void);

	bool LoadShader(const TCHAR *shaderFile);
	void Render(LPDIRECT3DTEXTURE9 &origTex, float2 textureSize, float2 inputSize, float2 viewportSize, float2 windowSize);
	void ClearPasses();	
	void OnLostDevice();
	void OnResetDevice();
};

#endif
