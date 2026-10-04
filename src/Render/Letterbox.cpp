#include "SD/Render/Letterbox.h"

#include "SD/Core/Logging.h"

#include <chrono>

namespace SD::Render
{
	namespace
	{
		using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);

		// IUnknown 0-2, IDXGIObject 3-6, IDXGIDeviceSubObject 7, Present 8.
		constexpr std::size_t kPresentIndex = 8;

		// Fraction of screen height each bar covers when fully out. Atomic because the
		// Present hook reads it on the render thread while the game thread writes it.
		std::atomic<float> barFraction{ 0.115f };
		constexpr float    kEaseSeconds = 0.32f;

		std::atomic<PresentFn> originalPresent{ nullptr };
		std::atomic_bool       installed{ false };
		std::atomic_bool       enabled{ true };
		std::atomic_bool       wantVisible{ false };

		// "Off the screen now, not eased off." See Letterbox::Retract.
		std::atomic_bool       snapClosed{ false };

		// A menu owns the screen, as MenuWatch sees it (not the pause counter). See
		// Letterbox::SetScreenTaken.
		std::atomic_bool       screenTaken{ false };

		ComPtr<ID3D11VertexShader> vertexShader;
		ComPtr<ID3D11PixelShader>  pixelShader;
		ComPtr<ID3D11InputLayout>  inputLayout;
		ComPtr<ID3D11Buffer>       vertexBuffer;
		ComPtr<ID3D11BlendState>   blendState;
		ComPtr<ID3D11DepthStencilState> depthState;
		ComPtr<ID3D11RasterizerState>   rasterState;
		ComPtr<ID3D11RenderTargetView>  renderTarget;
		bool                            resourcesReady{ false };

		// The back buffer's size, so a menu drawing into an off-screen texture isn't
		// given the bars.
		UINT frameWidth{ 0 };
		UINT frameHeight{ 0 };

		float                                 extension{ 0.0f };
		std::chrono::steady_clock::time_point lastDraw{};
		bool                                  haveLastDraw{ false };

		// The bottom bar as last drawn, as a fraction of the frame.
		std::atomic<float> drawnFraction{ 0.0f };

		// Under the interface or over it. Present runs after every menu has drawn, so
		// bars drawn there cover the interface. With subtitles in the bar, the bars
		// are drawn before the chosen menu's movie instead, so the menus draw on top.
		// Present still draws them on any frame where that didn't happen.
		std::atomic<Letterbox::Beneath> beneath{ Letterbox::Beneath::kOff };
		std::atomic<std::uint64_t>      presentFrame{ 0 };
		std::atomic<std::uint64_t>      beneathFrame{ ~0ull };

		// The frame each hooked menu last drew on, so the render order can be
		// reported: a HUD that draws after the dialogue menu draws over bars placed
		// under the dialogue menu.
		std::atomic<std::uint64_t> hudDrawnFrame{ ~0ull };
		Log::OnceFlag              orderReported;

		Log::OnceFlag firstDrawReported;
		Log::OnceFlag menuRetractReported;
		Log::OnceFlag beneathReported;
		Log::OnceFlag beneathUnboundReported;
		Log::OnceFlag beneathMismatchReported;

		constexpr char kShaderSource[] = R"(
struct VSIn  { float2 pos : POSITION; };
struct VSOut { float4 pos : SV_POSITION; };
VSOut VSMain(VSIn i) { VSOut o; o.pos = float4(i.pos, 0.0f, 1.0f); return o; }
float4 PSMain(VSOut i) : SV_TARGET { return float4(0.0f, 0.0f, 0.0f, 1.0f); }
)";

		void Disable(std::string_view a_reason)
		{
			enabled.store(false, std::memory_order_release);
			Log::Error(Log::Category::kRender, "Letterbox disabled for this session: {}"sv, a_reason);
		}

		[[nodiscard]] bool CreateResources(ID3D11Device* a_device, IDXGISwapChain* a_swapChain)
		{
			ComPtr<ID3DBlob> vsBlob, psBlob, errors;

			if (FAILED(::D3DCompile(kShaderSource, sizeof(kShaderSource) - 1, nullptr, nullptr, nullptr,
					"VSMain", "vs_5_0", 0, 0, &vsBlob, &errors))) {
				Disable("vertex shader compilation failed"sv);
				return false;
			}
			if (FAILED(::D3DCompile(kShaderSource, sizeof(kShaderSource) - 1, nullptr, nullptr, nullptr,
					"PSMain", "ps_5_0", 0, 0, &psBlob, &errors))) {
				Disable("pixel shader compilation failed"sv);
				return false;
			}

			if (FAILED(a_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vertexShader)) ||
				FAILED(a_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &pixelShader))) {
				Disable("shader creation failed"sv);
				return false;
			}

			const D3D11_INPUT_ELEMENT_DESC element{
				"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0
			};
			if (FAILED(a_device->CreateInputLayout(&element, 1, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &inputLayout))) {
				Disable("input layout creation failed"sv);
				return false;
			}

			D3D11_BUFFER_DESC bufferDesc{};
			bufferDesc.ByteWidth = sizeof(float) * 2 * 12;  // two quads, six vertices each
			bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
			bufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
			bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			if (FAILED(a_device->CreateBuffer(&bufferDesc, nullptr, &vertexBuffer))) {
				Disable("vertex buffer creation failed"sv);
				return false;
			}

			D3D11_BLEND_DESC blendDesc{};
			blendDesc.RenderTarget[0].BlendEnable = TRUE;
			blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
			blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
			blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
			blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
			blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
			blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
			blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
			if (FAILED(a_device->CreateBlendState(&blendDesc, &blendState))) {
				Disable("blend state creation failed"sv);
				return false;
			}

			D3D11_DEPTH_STENCIL_DESC depthDesc{};
			depthDesc.DepthEnable = FALSE;
			depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
			if (FAILED(a_device->CreateDepthStencilState(&depthDesc, &depthState))) {
				Disable("depth state creation failed"sv);
				return false;
			}

			D3D11_RASTERIZER_DESC rasterDesc{};
			rasterDesc.FillMode = D3D11_FILL_SOLID;
			rasterDesc.CullMode = D3D11_CULL_NONE;
			rasterDesc.DepthClipEnable = FALSE;
			if (FAILED(a_device->CreateRasterizerState(&rasterDesc, &rasterState))) {
				Disable("rasterizer state creation failed"sv);
				return false;
			}

			ComPtr<ID3D11Texture2D> backBuffer;
			if (FAILED(a_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))) ||
				FAILED(a_device->CreateRenderTargetView(backBuffer.Get(), nullptr, &renderTarget))) {
				Disable("render target view creation failed"sv);
				return false;
			}

			D3D11_TEXTURE2D_DESC frameDesc{};
			backBuffer->GetDesc(&frameDesc);
			frameWidth = frameDesc.Width;
			frameHeight = frameDesc.Height;

			resourcesReady = true;
			Log::Info(Log::Category::kRender, "Letterbox resources created."sv);
			return true;
		}

		void WriteBars(ID3D11DeviceContext* a_context, float a_height)
		{
			// Clip space: y = 1 at the top, -1 at the bottom, so a bar of a_height in
			// screen fractions is 2 * a_height tall here.
			const float h = a_height * 2.0f;
			const float top = 1.0f;
			const float topInner = 1.0f - h;
			const float bottom = -1.0f;
			const float bottomInner = -1.0f + h;

			const std::array<float, 24> vertices{
				-1.0f, top,   1.0f, top,   -1.0f, topInner,
				 1.0f, top,   1.0f, topInner, -1.0f, topInner,

				-1.0f, bottomInner, 1.0f, bottomInner, -1.0f, bottom,
				 1.0f, bottomInner, 1.0f, bottom,      -1.0f, bottom
			};

			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (SUCCEEDED(a_context->Map(vertexBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
				std::memcpy(mapped.pData, vertices.data(), sizeof(vertices));
				a_context->Unmap(vertexBuffer.Get(), 0);
			}
		}

		// One frame of the ease, run once per frame by whichever path draws. False
		// when there's nothing to draw; a_height is the bar to draw.
		[[nodiscard]] bool Advance(float& a_height)
		{
			// Ease toward the target so the bars slide rather than pop.
			const auto now = std::chrono::steady_clock::now();
			float      delta = 1.0f / 60.0f;
			if (haveLastDraw) {
				delta = std::clamp(std::chrono::duration<float>(now - lastDraw).count(), 0.0f, 0.25f);
			}
			lastDraw = now;
			haveLastDraw = true;

			// Retract immediately, from here, whenever a menu owns the screen. The
			// Director's tick stops while a pausing menu is up but Present doesn't, so
			// this has to be decided here.
			//
			// Not RE::UI::GameIsPaused(): that also counts the console, time-freezing
			// overlays and SKSE Menu Framework's own settings panel (FreezeTimeOnMenu),
			// which hid the bars while the player was adjusting them. `screenTaken` is
			// MenuWatch's answer (console excluded, CraftingMenu included); `snapClosed`
			// is the Director asking for the bars to go now rather than ease out.
			const bool taken = snapClosed.load(std::memory_order_acquire) ||
				screenTaken.load(std::memory_order_acquire);
			if (taken) {
				if (extension > 0.0f && menuRetractReported.Take()) {
					Log::Info(Log::Category::kRender,
						"Menu took the screen; bars retracted from the present hook."sv);
				}
				extension = 0.0f;
				drawnFraction.store(0.0f, std::memory_order_relaxed);
				return false;
			}

			const float target = wantVisible.load(std::memory_order_acquire) ? 1.0f : 0.0f;
			const float step = delta / kEaseSeconds;
			extension += std::clamp(target - extension, -step, step);

			if (extension <= 0.001f) {
				drawnFraction.store(0.0f, std::memory_order_relaxed);
				return false;  // fully retracted; touch nothing
			}

			a_height = barFraction.load(std::memory_order_relaxed) * extension;
			drawnFraction.store(a_height, std::memory_order_relaxed);
			return true;
		}

		// Draws the bars into a_target and restores everything it touched. a_viewport
		// is set when the caller isn't Present (whose viewport is already the whole
		// frame).
		void Render(ID3D11DeviceContext* context, ID3D11RenderTargetView* a_target,
			const D3D11_VIEWPORT* a_viewport, float a_height)
		{
			// Back up everything that's about to be overwritten. Drawing in Present
			// without restoring corrupts the game's next frame.
			ComPtr<ID3D11RenderTargetView> savedRTV;
			ComPtr<ID3D11DepthStencilView> savedDSV;
			context->OMGetRenderTargets(1, &savedRTV, &savedDSV);

			ComPtr<ID3D11BlendState> savedBlend;
			float                    savedBlendFactor[4]{};
			UINT                     savedSampleMask = 0;
			context->OMGetBlendState(&savedBlend, savedBlendFactor, &savedSampleMask);

			ComPtr<ID3D11DepthStencilState> savedDepth;
			UINT                            savedStencilRef = 0;
			context->OMGetDepthStencilState(&savedDepth, &savedStencilRef);

			ComPtr<ID3D11RasterizerState> savedRaster;
			context->RSGetState(&savedRaster);

			UINT               savedViewportCount = 1;
			D3D11_VIEWPORT     savedViewport{};
			context->RSGetViewports(&savedViewportCount, &savedViewport);

			ComPtr<ID3D11InputLayout> savedLayout;
			context->IAGetInputLayout(&savedLayout);
			D3D11_PRIMITIVE_TOPOLOGY savedTopology{};
			context->IAGetPrimitiveTopology(&savedTopology);

			ComPtr<ID3D11Buffer> savedVB;
			UINT                 savedStride = 0;
			UINT                 savedOffset = 0;
			context->IAGetVertexBuffers(0, 1, &savedVB, &savedStride, &savedOffset);

			ComPtr<ID3D11VertexShader> savedVS;
			ComPtr<ID3D11PixelShader>  savedPS;
			context->VSGetShader(&savedVS, nullptr, nullptr);
			context->PSGetShader(&savedPS, nullptr, nullptr);

			WriteBars(context, a_height);

			const UINT stride = sizeof(float) * 2;
			const UINT offset = 0;
			ID3D11RenderTargetView* rtv = a_target;
			const float             blendFactor[4]{ 0.0f, 0.0f, 0.0f, 0.0f };

			context->OMSetRenderTargets(1, &rtv, nullptr);
			if (a_viewport) {
				context->RSSetViewports(1, a_viewport);
			}
			context->OMSetBlendState(blendState.Get(), blendFactor, 0xFFFFFFFF);
			context->OMSetDepthStencilState(depthState.Get(), 0);
			context->RSSetState(rasterState.Get());
			context->IASetInputLayout(inputLayout.Get());
			context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			context->IASetVertexBuffers(0, 1, vertexBuffer.GetAddressOf(), &stride, &offset);
			context->VSSetShader(vertexShader.Get(), nullptr, 0);
			context->PSSetShader(pixelShader.Get(), nullptr, 0);
			context->Draw(12, 0);

			// Restore, in reverse.
			context->VSSetShader(savedVS.Get(), nullptr, 0);
			context->PSSetShader(savedPS.Get(), nullptr, 0);
			context->IASetVertexBuffers(0, 1, savedVB.GetAddressOf(), &savedStride, &savedOffset);
			context->IASetPrimitiveTopology(savedTopology);
			context->IASetInputLayout(savedLayout.Get());
			if (savedViewportCount > 0) {
				context->RSSetViewports(1, &savedViewport);
			}
			context->RSSetState(savedRaster.Get());
			context->OMSetDepthStencilState(savedDepth.Get(), savedStencilRef);
			context->OMSetBlendState(savedBlend.Get(), savedBlendFactor, savedSampleMask);
			context->OMSetRenderTargets(1, savedRTV.GetAddressOf(), savedDSV.Get());
		}

		[[nodiscard]] ID3D11DeviceContext* Context(ID3D11Device** a_device = nullptr)
		{
			auto* manager = RE::BSGraphics::Renderer::GetSingleton();
			if (!manager) {
				return nullptr;
			}
			auto& runtime = manager->GetRuntimeData();
			if (!runtime.forwarder || !runtime.context) {
				return nullptr;
			}
			if (a_device) {
				*a_device = reinterpret_cast<ID3D11Device*>(runtime.forwarder);
			}
			return reinterpret_cast<ID3D11DeviceContext*>(runtime.context);
		}

		void Draw(IDXGISwapChain* a_swapChain)
		{
			ID3D11Device* device = nullptr;
			auto*         context = Context(&device);
			if (!context) {
				return;
			}

			if (!resourcesReady && !CreateResources(device, a_swapChain)) {
				return;
			}

			float height = 0.0f;
			if (!Advance(height)) {
				return;
			}

			Render(context, renderTarget.Get(), nullptr, height);

			if (firstDrawReported.Take()) {
				Log::Info(Log::Category::kRender, "Letterbox drawing; bar height {:.1f}% of frame."sv,
					barFraction.load(std::memory_order_relaxed) * 100.0f);
			}
		}

		// The bars drawn before a menu rather than after everything, into whatever
		// target the interface is drawing to (an upscaler may give the interface its
		// own target). Once per frame, by whichever hooked menu renders first.
		void DrawBeneath(std::string_view a_menu)
		{
			const auto frame = presentFrame.load(std::memory_order_relaxed);
			if (beneathFrame.load(std::memory_order_relaxed) == frame || !resourcesReady) {
				return;
			}

			auto* context = Context();
			if (!context) {
				return;
			}

			ComPtr<ID3D11RenderTargetView> bound;
			context->OMGetRenderTargets(1, &bound, nullptr);
			if (!bound) {
				// Not marked as drawn, so Present draws this frame's bars on top.
				if (beneathUnboundReported.Take()) {
					Log::Warn(Log::Category::kRender,
						"No render target bound when the {} drew; bars stay over the interface."sv, a_menu);
				}
				return;
			}

			ComPtr<ID3D11Resource> resource;
			bound->GetResource(&resource);
			ComPtr<ID3D11Texture2D> texture;
			if (!resource || FAILED(resource.As(&texture)) || !texture) {
				return;
			}
			D3D11_TEXTURE2D_DESC desc{};
			texture->GetDesc(&desc);

			// Check the target's shape, not its size. A target with the frame's aspect is
			// the interface going to the screen, directly or through an upscaler (under
			// the PureDark upscaler the swap chain is 1280x720 while the UI draws to
			// 1920x1080). Anything else is an off-screen texture; that frame is left to
			// Present.
			const float frameAspect = frameHeight > 0 ? static_cast<float>(frameWidth) / static_cast<float>(frameHeight) : 0.0f;
			const float targetAspect = desc.Height > 0 ? static_cast<float>(desc.Width) / static_cast<float>(desc.Height) : 0.0f;
			const bool  frameShaped = frameAspect > 0.0f && desc.Width >= 640 &&
				std::abs(targetAspect - frameAspect) <= frameAspect * 0.02f;
			if (!frameShaped) {
				if (beneathMismatchReported.Take()) {
					Log::Warn(Log::Category::kRender,
						"The {} drew to a {}x{} target, not shaped like the {}x{} frame; bars stay over the interface on those frames."sv,
						a_menu, desc.Width, desc.Height, frameWidth, frameHeight);
				}
				return;
			}

			beneathFrame.store(frame, std::memory_order_relaxed);

			float height = 0.0f;
			if (!Advance(height)) {
				return;
			}

			const D3D11_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(desc.Width),
				static_cast<float>(desc.Height), 0.0f, 1.0f };
			Render(context, bound.Get(), &viewport, height);

			if (beneathReported.Take()) {
				Log::Info(Log::Category::kRender,
					"Letterbox drawing beneath the interface, from the {} ({}x{} target; swap chain {}x{})."sv,
					a_menu, desc.Width, desc.Height, frameWidth, frameHeight);
			}
		}

		// IMenu::PostDisplay, which draws the menu's movie. One instantiation per menu
		// so each keeps its own original.
		template <std::size_t Slot>
		struct PostDisplayHook
		{
			static void thunk(RE::IMenu* a_menu)
			{
				constexpr auto mine = Slot == 0 ? Letterbox::Beneath::kHud : Letterbox::Beneath::kDialogue;
				if (beneath.load(std::memory_order_acquire) == mine && enabled.load(std::memory_order_acquire)) {
					if constexpr (Slot == 1) {
						const auto frame = presentFrame.load(std::memory_order_relaxed);
						if (orderReported.Take()) {
							Log::Info(Log::Category::kRender, "The HUD draws {} the dialogue menu{}."sv,
								hudDrawnFrame.load(std::memory_order_relaxed) == frame ? "before"sv : "after"sv,
								hudDrawnFrame.load(std::memory_order_relaxed) == frame ?
									"; HUD elements stay under the bars"sv :
									"; HUD elements will show over the bars"sv);
						}
					}
					try {
						DrawBeneath(Slot == 0 ? "HUD"sv : "dialogue menu"sv);
					} catch (...) {
						Disable("drawing beneath the interface threw an exception"sv);
					}
				}
				func(a_menu);
				if constexpr (Slot == 0) {
					hudDrawnFrame.store(presentFrame.load(std::memory_order_relaxed), std::memory_order_relaxed);
				}
			}

			static inline REL::Relocation<decltype(thunk)> func;
		};

		constexpr std::size_t kPostDisplayIndex = 0x6;

		HRESULT STDMETHODCALLTYPE DetourPresent(IDXGISwapChain* a_swapChain, UINT a_sync, UINT a_flags)
		{
			const auto original = originalPresent.load(std::memory_order_acquire);

			if (enabled.load(std::memory_order_acquire)) {
				const bool drawnBeneath = beneath.load(std::memory_order_acquire) != Letterbox::Beneath::kOff &&
					beneathFrame.load(std::memory_order_relaxed) == presentFrame.load(std::memory_order_relaxed);
				if (!drawnBeneath) {
					try {
						Draw(a_swapChain);
					} catch (...) {
						Disable("draw threw an exception"sv);
					}
				}
			}

			presentFrame.fetch_add(1, std::memory_order_relaxed);
			return original ? original(a_swapChain, a_sync, a_flags) : E_FAIL;
		}
	}

	void Letterbox::Install()
	{
		if (installed.load(std::memory_order_relaxed)) {
			return;
		}

		auto* manager = RE::BSGraphics::Renderer::GetSingleton();
		if (!manager) {
			Log::Error(Log::Category::kRender, "Renderer unavailable; no letterbox."sv);
			return;
		}

		auto& runtime = manager->GetRuntimeData();
		auto* swapChain = reinterpret_cast<IDXGISwapChain*>(runtime.renderWindows[0].swapChain);
		if (!swapChain) {
			Log::Error(Log::Category::kRender, "Swap chain unavailable; no letterbox."sv);
			return;
		}

		auto** vtable = *reinterpret_cast<void***>(swapChain);
		if (!vtable) {
			Log::Error(Log::Category::kRender, "Swap chain vtable unreadable; no letterbox."sv);
			return;
		}

		void** slot = &vtable[kPresentIndex];
		DWORD  protection = 0;
		if (!::VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &protection)) {
			Log::Error(Log::Category::kRender, "Could not unprotect the Present slot; no letterbox."sv);
			return;
		}

		originalPresent.store(reinterpret_cast<PresentFn>(*slot), std::memory_order_release);
		*slot = reinterpret_cast<void*>(&DetourPresent);

		DWORD ignored = 0;
		::VirtualProtect(slot, sizeof(void*), protection, &ignored);

		// Installed at load rather than when the setting is first ticked, so the
		// vtable isn't written while that menu is drawing. With the setting off each
		// hook is one atomic load.
		REL::Relocation<std::uintptr_t> hud{ RE::VTABLE_HUDMenu[0] };
		PostDisplayHook<0>::func = hud.write_vfunc(kPostDisplayIndex, PostDisplayHook<0>::thunk);
		REL::Relocation<std::uintptr_t> dialogue{ RE::VTABLE_DialogueMenu[0] };
		PostDisplayHook<1>::func = dialogue.write_vfunc(kPostDisplayIndex, PostDisplayHook<1>::thunk);

		installed.store(true, std::memory_order_release);
		Log::Info(Log::Category::kRender,
			"Present hook installed for the letterbox; HUD and dialogue menu hooks ready for bars beneath the interface."sv);
	}

	void Letterbox::SetBeneath(Beneath a_menu)
	{
		if (beneath.exchange(a_menu, std::memory_order_acq_rel) != a_menu) {
			Log::Info(Log::Category::kRender, "Letterbox now draws {}."sv,
				a_menu == Beneath::kDialogue ? "beneath the dialogue menu"sv :
				a_menu == Beneath::kHud      ? "beneath the HUD"sv :
											   "over the interface"sv);
		}
	}

	float Letterbox::DrawnFraction() noexcept
	{
		return drawnFraction.load(std::memory_order_relaxed);
	}

	float Letterbox::TargetFraction() noexcept
	{
		const bool shown = enabled.load(std::memory_order_acquire) &&
			wantVisible.load(std::memory_order_acquire) &&
			!snapClosed.load(std::memory_order_acquire) &&
			!screenTaken.load(std::memory_order_acquire);
		return shown ? barFraction.load(std::memory_order_relaxed) : 0.0f;
	}

	void Letterbox::Shutdown()
	{
		// Retract the bars rather than remove the hook; the render thread may be
		// inside the detour.
		wantVisible.store(false, std::memory_order_release);
		enabled.store(false, std::memory_order_release);
	}

	void Letterbox::SetBarFraction(float a_fraction)
	{
		// Capped well below half, so the bars can't black out the frame.
		barFraction.store(std::clamp(a_fraction, 0.0f, 0.30f), std::memory_order_relaxed);
	}

	void Letterbox::SetVisible(bool a_visible)
	{
		// Asking for the bars clears the snap, so a conversation resuming after a
		// trade eases them back in normally.
		if (a_visible) {
			snapClosed.store(false, std::memory_order_release);
		}
		wantVisible.store(a_visible, std::memory_order_release);
	}

	void Letterbox::Retract()
	{
		wantVisible.store(false, std::memory_order_release);
		snapClosed.store(true, std::memory_order_release);
	}

	void Letterbox::SetScreenTaken(bool a_taken)
	{
		screenTaken.store(a_taken, std::memory_order_release);
	}

	bool Letterbox::Installed() noexcept
	{
		return installed.load(std::memory_order_acquire);
	}
}
