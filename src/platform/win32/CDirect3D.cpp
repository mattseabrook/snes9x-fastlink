/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "CDirect3D.h"
#include "win32_display.h"
#include "core/util/snes9x.h"
#include "core/video-common/gfx.h"
#include "core/video-common/display.h"
#include "wsnes9x.h"

#include <d3dcompiler.h>

#include "snes9x_imgui.h"

#include <algorithm>
#include <cstring>

#include "filter/hq2x.h"
#include "filter/2xsai.h"

namespace {

static const char *kVertexShaderSource =
"struct VSInput { float2 pos : POSITION; float2 uv : TEXCOORD0; };"
"struct VSOutput { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };"
"VSOutput main(VSInput input) {"
"  VSOutput output;"
"  output.pos = float4(input.pos.x, input.pos.y, 0.0f, 1.0f);"
"  output.uv = input.uv;"
"  return output;"
"}";

static const char *kPixelShaderSource =
"Texture2D screenTex : register(t0);"
"SamplerState screenSampler : register(s0);"
"float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_Target {"
"  return screenTex.Sample(screenSampler, uv);"
"}";

static UINT EffectiveBufferCount()
{
	return 2;
}

static DXGI_FORMAT FullscreenFormatForDepth(int depth)
{
	if (depth == 16)
		return DXGI_FORMAT_B5G6R5_UNORM;
	return DXGI_FORMAT_R8G8B8A8_UNORM;
}

} // namespace

CDirect3D::CDirect3D()
{
	init_done = false;
	hWnd = nullptr;
	pFactory = nullptr;
	pSwapChain = nullptr;
	pDevice = nullptr;
	pContext = nullptr;
	pRenderTargetView = nullptr;
	drawSurface = nullptr;
	drawSurfaceSRV = nullptr;
	vertexBuffer = nullptr;
	vertexShader = nullptr;
	pixelShader = nullptr;
	inputLayout = nullptr;
	pointSampler = nullptr;
	linearSampler = nullptr;
	latencyQuery = nullptr;
	backBufferWidth = 0;
	backBufferHeight = 0;
	filterScale = 1;
	afterRenderWidth = 0;
	afterRenderHeight = 0;
	quadTextureSize = 0;
	fullscreen = false;
	ZeroMemory(&swapChainDesc, sizeof(swapChainDesc));
}

CDirect3D::~CDirect3D()
{
	DeInitialize();
}

bool CDirect3D::CreateShaders()
{
	ID3DBlob *vsBlob = nullptr;
	ID3DBlob *psBlob = nullptr;
	ID3DBlob *errors = nullptr;

	HRESULT hr = D3DCompile(
		kVertexShaderSource,
		strlen(kVertexShaderSource),
		nullptr,
		nullptr,
		nullptr,
		"main",
		"vs_4_0",
		0,
		0,
		&vsBlob,
		&errors);

	if (FAILED(hr))
	{
		if (errors)
			errors->Release();
		MessageBox(nullptr, TEXT("Error compiling D3D11 vertex shader"), TEXT("Error"), MB_ICONERROR | MB_OK);
		return false;
	}

	hr = pDevice->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vertexShader);
	if (FAILED(hr))
	{
		vsBlob->Release();
		MessageBox(nullptr, TEXT("Error creating D3D11 vertex shader"), TEXT("Error"), MB_ICONERROR | MB_OK);
		return false;
	}

	D3D11_INPUT_ELEMENT_DESC layout[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 }
	};

	hr = pDevice->CreateInputLayout(layout, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &inputLayout);
	vsBlob->Release();
	if (FAILED(hr))
	{
		MessageBox(nullptr, TEXT("Error creating D3D11 input layout"), TEXT("Error"), MB_ICONERROR | MB_OK);
		return false;
	}

	hr = D3DCompile(
		kPixelShaderSource,
		strlen(kPixelShaderSource),
		nullptr,
		nullptr,
		nullptr,
		"main",
		"ps_4_0",
		0,
		0,
		&psBlob,
		&errors);

	if (FAILED(hr))
	{
		if (errors)
			errors->Release();
		MessageBox(nullptr, TEXT("Error compiling D3D11 pixel shader"), TEXT("Error"), MB_ICONERROR | MB_OK);
		return false;
	}

	hr = pDevice->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &pixelShader);
	psBlob->Release();
	if (FAILED(hr))
	{
		MessageBox(nullptr, TEXT("Error creating D3D11 pixel shader"), TEXT("Error"), MB_ICONERROR | MB_OK);
		return false;
	}

	return true;
}

bool CDirect3D::CreateStates()
{
	D3D11_BUFFER_DESC vbDesc = {};
	vbDesc.Usage = D3D11_USAGE_DYNAMIC;
	vbDesc.ByteWidth = sizeof(vertexStream);
	vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
	vbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

	if (FAILED(pDevice->CreateBuffer(&vbDesc, nullptr, &vertexBuffer)))
	{
		MessageBox(nullptr, TEXT("Error creating D3D11 vertex buffer"), TEXT("Error"), MB_ICONERROR | MB_OK);
		return false;
	}

	D3D11_SAMPLER_DESC samplerDesc = {};
	samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
	samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
	samplerDesc.MinLOD = 0;
	samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;

	if (FAILED(pDevice->CreateSamplerState(&samplerDesc, &pointSampler)))
	{
		MessageBox(nullptr, TEXT("Error creating D3D11 point sampler"), TEXT("Error"), MB_ICONERROR | MB_OK);
		return false;
	}

	samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	if (FAILED(pDevice->CreateSamplerState(&samplerDesc, &linearSampler)))
	{
		MessageBox(nullptr, TEXT("Error creating D3D11 linear sampler"), TEXT("Error"), MB_ICONERROR | MB_OK);
		return false;
	}

	D3D11_QUERY_DESC queryDesc = {};
	queryDesc.Query = D3D11_QUERY_EVENT;
	pDevice->CreateQuery(&queryDesc, &latencyQuery);

	return true;
}

void CDirect3D::CreateRenderTarget()
{
	DestroyRenderTarget();

	ID3D11Texture2D *backBuffer = nullptr;
	if (FAILED(pSwapChain->GetBuffer(0, IID_ID3D11Texture2D, reinterpret_cast<void **>(&backBuffer))))
		return;

	pDevice->CreateRenderTargetView(backBuffer, nullptr, &pRenderTargetView);

	D3D11_TEXTURE2D_DESC desc = {};
	backBuffer->GetDesc(&desc);
	backBufferWidth = desc.Width;
	backBufferHeight = desc.Height;
	backBuffer->Release();
}

void CDirect3D::DestroyRenderTarget()
{
	if (pRenderTargetView)
	{
		pRenderTargetView->Release();
		pRenderTargetView = nullptr;
	}
}

void CDirect3D::ReleaseDevice()
{
	DestroyDrawSurface();
	DestroyRenderTarget();

	if (latencyQuery)
	{
		latencyQuery->Release();
		latencyQuery = nullptr;
	}
	if (linearSampler)
	{
		linearSampler->Release();
		linearSampler = nullptr;
	}
	if (pointSampler)
	{
		pointSampler->Release();
		pointSampler = nullptr;
	}
	if (inputLayout)
	{
		inputLayout->Release();
		inputLayout = nullptr;
	}
	if (pixelShader)
	{
		pixelShader->Release();
		pixelShader = nullptr;
	}
	if (vertexShader)
	{
		vertexShader->Release();
		vertexShader = nullptr;
	}
	if (vertexBuffer)
	{
		vertexBuffer->Release();
		vertexBuffer = nullptr;
	}
	if (pSwapChain)
	{
		pSwapChain->SetFullscreenState(FALSE, nullptr);
		pSwapChain->Release();
		pSwapChain = nullptr;
	}
	if (pContext)
	{
		pContext->Release();
		pContext = nullptr;
	}
	if (pDevice)
	{
		pDevice->Release();
		pDevice = nullptr;
	}
	if (pFactory)
	{
		pFactory->Release();
		pFactory = nullptr;
	}
}

bool CDirect3D::Initialize(HWND window)
{
	if (init_done)
		return true;

	hWnd = window;

	RECT client = {};
	GetClientRect(hWnd, &client);
	const UINT width = std::max<UINT>(1, client.right - client.left);
	const UINT height = std::max<UINT>(1, client.bottom - client.top);

	DXGI_SWAP_CHAIN_DESC desc = {};
	desc.BufferCount = EffectiveBufferCount();
	desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.BufferDesc.Width = width;
	desc.BufferDesc.Height = height;
	desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	desc.OutputWindow = hWnd;
	desc.SampleDesc.Count = 1;
	desc.Windowed = TRUE;
	desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

	UINT createFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
	D3D_FEATURE_LEVEL featureLevels[] = {
		D3D_FEATURE_LEVEL_11_1,
		D3D_FEATURE_LEVEL_11_0,
		D3D_FEATURE_LEVEL_10_1,
		D3D_FEATURE_LEVEL_10_0
	};
	D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;

	HRESULT hr = D3D11CreateDeviceAndSwapChain(
		nullptr,
		D3D_DRIVER_TYPE_HARDWARE,
		nullptr,
		createFlags,
		featureLevels,
		ARRAYSIZE(featureLevels),
		D3D11_SDK_VERSION,
		&desc,
		&pSwapChain,
		&pDevice,
		&featureLevel,
		&pContext);

	if (FAILED(hr))
	{
		hr = D3D11CreateDeviceAndSwapChain(
			nullptr,
			D3D_DRIVER_TYPE_WARP,
			nullptr,
			createFlags,
			featureLevels,
			ARRAYSIZE(featureLevels),
			D3D11_SDK_VERSION,
			&desc,
			&pSwapChain,
			&pDevice,
			&featureLevel,
			&pContext);
	}

	if (FAILED(hr) || !pSwapChain || !pDevice || !pContext)
	{
		MessageBox(nullptr, TEXT("Error creating D3D11 device"), TEXT("Error"), MB_ICONERROR | MB_OK);
		ReleaseDevice();
		return false;
	}

	IDXGIDevice *dxgiDevice = nullptr;
	if (SUCCEEDED(pDevice->QueryInterface(IID_IDXGIDevice, reinterpret_cast<void **>(&dxgiDevice))))
	{
		IDXGIAdapter *adapter = nullptr;
		if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter)))
		{
			adapter->GetParent(IID_IDXGIFactory1, reinterpret_cast<void **>(&pFactory));
			adapter->Release();
		}
		dxgiDevice->Release();
	}

	swapChainDesc = desc;
	CreateRenderTarget();

	if (!pRenderTargetView || !CreateShaders() || !CreateStates())
	{
		ReleaseDevice();
		return false;
	}

	Clear();

	init_done = true;
	ApplyDisplayChanges();

	return true;
}

void CDirect3D::DeInitialize()
{
	ReleaseDevice();

	init_done = false;
	afterRenderWidth = 0;
	afterRenderHeight = 0;
	quadTextureSize = 0;
	fullscreen = false;
	filterScale = 0;
	backBufferWidth = 0;
	backBufferHeight = 0;
}

bool CDirect3D::SetShader(const TCHAR *file)
{
	if (file && lstrlen(file) > 0)
	{
		if (GUI.shaderEnabled)
		{
			MessageBox(nullptr, TEXT("Direct3D now uses a fixed D3D11 shader path; external D3D shader files are ignored."), TEXT("Direct3D11"), MB_ICONINFORMATION | MB_OK);
		}
	}

	return true;
}

void CDirect3D::Render(SSurface Src)
{
	if (!init_done)
		return;

	if (!drawSurface || !drawSurfaceSRV)
	{
		const int maxFilterScale = std::max(GetFilterScale(GUI.ScaleHiRes), GetFilterScale(GUI.Scale));
		const unsigned int newFilterScale = static_cast<unsigned int>(std::max(2, maxFilterScale));
		ChangeDrawSurfaceSize(newFilterScale);
	}

	const int maxFilterScale = std::max(GetFilterScale(GUI.ScaleHiRes), GetFilterScale(GUI.Scale));
	const unsigned int newFilterScale = static_cast<unsigned int>(std::max(2, maxFilterScale));
	if (newFilterScale != filterScale)
		ChangeDrawSurfaceSize(newFilterScale);

	if (!drawSurface || !drawSurfaceSRV)
		return;

	D3D11_MAPPED_SUBRESOURCE mapped = {};
	if (FAILED(pContext->Map(drawSurface, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
	{
		MessageBox(nullptr, TEXT("Unable to map D3D11 texture"), TEXT("Error"), MB_ICONERROR | MB_OK);
		return;
	}

	SSurface Dst;
	RECT dstRect = {};
	Dst.Surface = reinterpret_cast<uint8_t *>(mapped.pData);
	Dst.Height = quadTextureSize;
	Dst.Width = quadTextureSize;
	Dst.Pitch = mapped.RowPitch;

	RenderMethod(Src, Dst, &dstRect);

	pContext->Unmap(drawSurface, 0);

	if (afterRenderHeight != (unsigned int)dstRect.bottom || afterRenderWidth != (unsigned int)dstRect.right)
	{
		afterRenderHeight = dstRect.bottom;
		afterRenderWidth = dstRect.right;
		SetViewport();
	}

	if (!GUI.Stretch || GUI.AspectRatio)
		Clear();

	const FLOAT blendFactor[4] = {0.f, 0.f, 0.f, 0.f};
	pContext->OMSetBlendState(nullptr, blendFactor, 0xffffffff);
	pContext->OMSetRenderTargets(1, &pRenderTargetView, nullptr);

	UINT stride = sizeof(VERTEX);
	UINT offset = 0;
	pContext->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
	pContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	pContext->IASetInputLayout(inputLayout);

	pContext->VSSetShader(vertexShader, nullptr, 0);
	pContext->PSSetShader(pixelShader, nullptr, 0);
	pContext->PSSetShaderResources(0, 1, &drawSurfaceSRV);
	SetFiltering();

	pContext->Draw(4, 0);

	ID3D11ShaderResourceView *nullSRV = nullptr;
	pContext->PSSetShaderResources(0, 1, &nullSRV);

	WinLatencyMarkPresentSubmit();
	pSwapChain->Present(GUI.Vsync ? 1 : 0, 0);

	if (!GUI.Vsync && !GUI.DWMSync)
		WaitForLowLagSync();
}

void CDirect3D::CreateDrawSurface()
{
	unsigned int neededSize = SNES_WIDTH * filterScale;
	quadTextureSize = 512;
	while (quadTextureSize < neededSize)
		quadTextureSize *= 2;

	if (drawSurface)
		return;

	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = quadTextureSize;
	desc.Height = quadTextureSize;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_B5G6R5_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DYNAMIC;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

	if (FAILED(pDevice->CreateTexture2D(&desc, nullptr, &drawSurface)))
	{
		MessageBox(nullptr, TEXT("Error while creating D3D11 texture"), TEXT("Error"), MB_ICONERROR | MB_OK);
		return;
	}

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Format = desc.Format;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MipLevels = 1;
	pDevice->CreateShaderResourceView(drawSurface, &srvDesc, &drawSurfaceSRV);
}

void CDirect3D::DestroyDrawSurface()
{
	if (drawSurfaceSRV)
	{
		drawSurfaceSRV->Release();
		drawSurfaceSRV = nullptr;
	}
	if (drawSurface)
	{
		drawSurface->Release();
		drawSurface = nullptr;
	}
}

bool CDirect3D::BlankTexture(ID3D11Texture2D *texture)
{
	if (!texture)
		return false;

	D3D11_MAPPED_SUBRESOURCE mapped = {};
	if (FAILED(pContext->Map(texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
		return false;

	memset(mapped.pData, 0, mapped.RowPitch * quadTextureSize);
	pContext->Unmap(texture, 0);
	return true;
}

bool CDirect3D::ChangeDrawSurfaceSize(unsigned int scale)
{
	filterScale = scale;

	if (!pDevice)
		return false;

	DestroyDrawSurface();
	CreateDrawSurface();
	SetupVertices();

	return drawSurface != nullptr;
}

void CDirect3D::SetupVertices()
{
	if (!vertexBuffer || backBufferWidth == 0 || backBufferHeight == 0)
		return;

	float tX = static_cast<float>(afterRenderWidth) / static_cast<float>(quadTextureSize);
	float tY = static_cast<float>(afterRenderHeight) / static_cast<float>(quadTextureSize);

	RECT drawRect = CalculateDisplayRect(afterRenderWidth, afterRenderHeight, backBufferWidth, backBufferHeight);
	float left = static_cast<float>(drawRect.left) / static_cast<float>(backBufferWidth) * 2.0f - 1.0f;
	float right = static_cast<float>(drawRect.right) / static_cast<float>(backBufferWidth) * 2.0f - 1.0f;
	float top = 1.0f - static_cast<float>(drawRect.top) / static_cast<float>(backBufferHeight) * 2.0f;
	float bottom = 1.0f - static_cast<float>(drawRect.bottom) / static_cast<float>(backBufferHeight) * 2.0f;

	vertexStream[0] = VERTEX(left,  top,    0.0f, 0.0f);
	vertexStream[1] = VERTEX(left,  bottom, 0.0f, tY);
	vertexStream[2] = VERTEX(right, top,    tX,   0.0f);
	vertexStream[3] = VERTEX(right, bottom, tX,   tY);

	D3D11_MAPPED_SUBRESOURCE mapped = {};
	if (SUCCEEDED(pContext->Map(vertexBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
	{
		memcpy(mapped.pData, vertexStream, sizeof(vertexStream));
		pContext->Unmap(vertexBuffer, 0);
	}
}

void CDirect3D::SetViewport()
{
	if (!pContext || backBufferWidth == 0 || backBufferHeight == 0)
		return;

	D3D11_VIEWPORT vp = {};
	vp.TopLeftX = 0.0f;
	vp.TopLeftY = 0.0f;
	vp.Width = static_cast<FLOAT>(backBufferWidth);
	vp.Height = static_cast<FLOAT>(backBufferHeight);
	vp.MinDepth = 0.0f;
	vp.MaxDepth = 1.0f;
	pContext->RSSetViewports(1, &vp);

	SetupVertices();
}

bool CDirect3D::ChangeRenderSize(unsigned int newWidth, unsigned int newHeight)
{
	if (!init_done)
		return false;

	if (backBufferWidth == newWidth && backBufferHeight == newHeight)
		return true;

	return ResetDevice();
}

bool CDirect3D::ResetDevice()
{
	if (!init_done || !pSwapChain)
		return false;

	pContext->OMSetRenderTargets(0, nullptr, nullptr);
	DestroyRenderTarget();

	UINT width = 0;
	UINT height = 0;
	if (fullscreen)
	{
		width = GUI.FullscreenMode.width;
		height = GUI.FullscreenMode.height;
	}
	else
	{
		RECT client = {};
		GetClientRect(hWnd, &client);
		width = std::max<UINT>(1, client.right - client.left);
		height = std::max<UINT>(1, client.bottom - client.top);
	}

	if (FAILED(pSwapChain->ResizeBuffers(EffectiveBufferCount(), width, height, DXGI_FORMAT_R8G8B8A8_UNORM, 0)))
	{
		MessageBox(nullptr, TEXT("Unable to resize D3D11 swapchain"), TEXT("Error"), MB_ICONERROR | MB_OK);
		return false;
	}

	if (fullscreen)
	{
		DXGI_MODE_DESC modeDesc = {};
		modeDesc.Width = width;
		modeDesc.Height = height;
		modeDesc.Format = FullscreenFormatForDepth(GUI.FullscreenMode.depth);
		modeDesc.RefreshRate.Numerator = GUI.FullscreenMode.rate;
		modeDesc.RefreshRate.Denominator = 1;
		pSwapChain->ResizeTarget(&modeDesc);
		pSwapChain->SetFullscreenState(TRUE, nullptr);
	}
	else
	{
		pSwapChain->SetFullscreenState(FALSE, nullptr);
	}

	CreateRenderTarget();
	CreateDrawSurface();
	SetViewport();

	return pRenderTargetView != nullptr;
}

void CDirect3D::WaitForLowLagSync()
{
	if (!latencyQuery)
		return;

	pContext->End(latencyQuery);
	while (S_FALSE == pContext->GetData(latencyQuery, nullptr, 0, 0))
	{
		SwitchToThread();
	}
}

void CDirect3D::SetSnes9xColorFormat()
{
	GUI.ScreenDepth = 16;
	GUI.BlueShift = 0;
	GUI.GreenShift = 6;
	GUI.RedShift = 11;
	S9xBlit2xSaIFilterInit();
	S9xBlitHQ2xFilterInit();
	GUI.NeedDepthConvert = FALSE;
	GUI.DepthConverted = TRUE;
}

bool CDirect3D::SetFullscreen(bool isFullscreen)
{
	if (!init_done)
		return false;

	if (fullscreen == isFullscreen)
		return true;

	fullscreen = isFullscreen;
	if (!ResetDevice())
		return false;

	WinLatencyMarkPresentSubmit();
	pSwapChain->Present(0, 0);

	return true;
}

void CDirect3D::EnumModes(std::vector<dMode> *modeVector)
{
	if (!modeVector)
		return;

	DEVMODE devMode = {};
	devMode.dmSize = sizeof(devMode);
	for (DWORD i = 0; EnumDisplaySettings(nullptr, i, &devMode); i++)
	{
		if (devMode.dmBitsPerPel != 16 && devMode.dmBitsPerPel != 32)
			continue;

		dMode mode;
		mode.width = devMode.dmPelsWidth;
		mode.height = devMode.dmPelsHeight;
		mode.rate = devMode.dmDisplayFrequency;
		mode.depth = devMode.dmBitsPerPel;
		modeVector->push_back(mode);
	}
}

bool CDirect3D::ApplyDisplayChanges(void)
{
	if (GUI.shaderEnabled && GUI.D3DshaderFileName[0] != '\0')
		SetShader(GUI.D3DshaderFileName);
	else
		SetShader(nullptr);

	return ChangeRenderSize(0, 0);
}

void CDirect3D::SetFiltering()
{
	ID3D11SamplerState *sampler = Settings.BilinearFilter ? linearSampler : pointSampler;
	pContext->PSSetSamplers(0, 1, &sampler);
}

void CDirect3D::Clear()
{
	if (!init_done || !pRenderTargetView)
		return;

	const FLOAT black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	pContext->OMSetRenderTargets(1, &pRenderTargetView, nullptr);
	pContext->ClearRenderTargetView(pRenderTargetView, black);
}
