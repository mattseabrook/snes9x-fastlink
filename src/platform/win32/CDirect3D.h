/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#define MAX_SHADER_TEXTURES 8

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d11.h>
#include <dxgi.h>

#include "render.h"
#include "wsnes9x.h"
#include "IS9xDisplayOutput.h"

typedef struct _VERTEX {
		float x, y;
		float tx, ty;
		_VERTEX() {}
		_VERTEX(float x,float y,float tx,float ty) {
			this->x=x;this->y=y;this->tx=tx;this->ty=ty;
		}
} VERTEX; //our custom vertex with a constuctor for easier assignment

class CDirect3D: public IS9xDisplayOutput
{
private:
	bool                  init_done;					//has initialize been called?
	HWND                  hWnd;
	IDXGIFactory1*        pFactory;
	IDXGISwapChain*       pSwapChain;
	ID3D11Device*         pDevice;
	ID3D11DeviceContext*  pContext;
	ID3D11RenderTargetView* pRenderTargetView;
	ID3D11Texture2D*      drawSurface;
	ID3D11ShaderResourceView* drawSurfaceSRV;

	ID3D11Buffer*         vertexBuffer;
	ID3D11VertexShader*   vertexShader;
	ID3D11PixelShader*    pixelShader;
	ID3D11InputLayout*    inputLayout;
	ID3D11SamplerState*   pointSampler;
	ID3D11SamplerState*   linearSampler;
	ID3D11Query*          latencyQuery;
	DXGI_SWAP_CHAIN_DESC  swapChainDesc;
	unsigned int          backBufferWidth;
	unsigned int          backBufferHeight;

	unsigned int filterScale;							//the current maximum filter scale (at least 2)
	unsigned int afterRenderWidth, afterRenderHeight;	//dimensions after filter has been applied
	unsigned int quadTextureSize;						//size of the texture (only multiples of 2)
	bool fullscreen;									//are we currently displaying in fullscreen mode
	
	VERTEX vertexStream[4];								//the 4 vertices that make up our display rectangle

	bool BlankTexture(ID3D11Texture2D *texture);
	void CreateDrawSurface();
	void DestroyDrawSurface();
	bool ChangeDrawSurfaceSize(unsigned int scale);
	void CreateRenderTarget();
	void DestroyRenderTarget();
	bool CreateShaders();
	bool CreateStates();
	void ReleaseDevice();
	void SetViewport();
	void SetupVertices();
	bool ResetDevice();
	void SetFiltering();
	bool SetShader(const TCHAR *file);
	void Clear();
	void WaitForLowLagSync();

public:
	CDirect3D();
	~CDirect3D();
	bool Initialize(HWND hWnd) override;
	void DeInitialize() override;
	void Render(SSurface Src) override;
	bool ChangeRenderSize(unsigned int newWidth, unsigned int newHeight) override;
	bool ApplyDisplayChanges(void) override;
	bool SetFullscreen(bool fullscreen) override;
	void SetSnes9xColorFormat() override;
	void EnumModes(std::vector<dMode> *modeVector) override;
};