#include "image.h"

#include <windows.h>
#include <wincodec.h>

namespace hl {

namespace {

template<typename T> struct Com {
	T *p = nullptr;
	~Com()
	{
		if (p)
			p->Release();
	}
	T **operator&() { return &p; }
	T *operator->() const { return p; }
	explicit operator bool() const { return p != nullptr; }
};

struct ComInit {
	bool owns = false;
	ComInit()
	{
		HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		owns = SUCCEEDED(hr);
	}
	~ComInit()
	{
		if (owns)
			CoUninitialize();
	}
};

} // namespace

bool write_image(const std::wstring &path, const uint8_t *bgra, int width, int height, int stride,
		 const std::string &format, float jpeg_quality)
{
	ComInit com;
	Com<IWICImagingFactory> factory;
	if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
		return false;

	Com<IWICStream> stream;
	if (FAILED(factory->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)))
		return false;

	const GUID container = format == "jpg" ? GUID_ContainerFormatJpeg : GUID_ContainerFormatPng;
	Com<IWICBitmapEncoder> encoder;
	if (FAILED(factory->CreateEncoder(container, nullptr, &encoder)) ||
	    FAILED(encoder->Initialize(stream.p, WICBitmapEncoderNoCache)))
		return false;

	Com<IWICBitmapFrameEncode> frame;
	Com<IPropertyBag2> props;
	if (FAILED(encoder->CreateNewFrame(&frame, &props)))
		return false;
	if (format == "jpg" && props) {
		PROPBAG2 opt = {};
		wchar_t name[] = L"ImageQuality";
		opt.pstrName = name;
		VARIANT v;
		VariantInit(&v);
		v.vt = VT_R4;
		v.fltVal = jpeg_quality;
		props->Write(1, &opt, &v);
	}
	if (FAILED(frame->Initialize(props.p)) || FAILED(frame->SetSize((UINT)width, (UINT)height)))
		return false;

	WICPixelFormatGUID pf = format == "jpg" ? GUID_WICPixelFormat24bppBGR : GUID_WICPixelFormat32bppBGRA;
	if (FAILED(frame->SetPixelFormat(&pf)))
		return false;

	if (IsEqualGUID(pf, GUID_WICPixelFormat32bppBGRA)) {
		if (FAILED(frame->WritePixels((UINT)height, (UINT)stride, (UINT)(stride * height), (BYTE *)bgra)))
			return false;
	} else {
		/* JPEG has no alpha: wrap the pixels and let WIC convert. */
		Com<IWICBitmap> bmp;
		if (FAILED(factory->CreateBitmapFromMemory((UINT)width, (UINT)height, GUID_WICPixelFormat32bppBGRA,
							   (UINT)stride, (UINT)(stride * height), (BYTE *)bgra, &bmp)))
			return false;
		Com<IWICFormatConverter> conv;
		if (FAILED(factory->CreateFormatConverter(&conv)) ||
		    FAILED(conv->Initialize(bmp.p, pf, WICBitmapDitherTypeNone, nullptr, 0.0,
					    WICBitmapPaletteTypeCustom)))
			return false;
		if (FAILED(frame->WriteSource(conv.p, nullptr)))
			return false;
	}

	return SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
}

bool read_image(const std::wstring &path, std::vector<uint8_t> &bgra, int &width, int &height)
{
	ComInit com;
	Com<IWICImagingFactory> factory;
	if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
		return false;

	Com<IWICBitmapDecoder> decoder;
	if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
						      WICDecodeMetadataCacheOnDemand, &decoder)))
		return false;
	Com<IWICBitmapFrameDecode> frame;
	if (FAILED(decoder->GetFrame(0, &frame)))
		return false;
	Com<IWICFormatConverter> conv;
	if (FAILED(factory->CreateFormatConverter(&conv)) ||
	    FAILED(conv->Initialize(frame.p, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
				    WICBitmapPaletteTypeCustom)))
		return false;
	UINT w = 0, h = 0;
	conv->GetSize(&w, &h);
	width = (int)w;
	height = (int)h;
	bgra.resize((size_t)w * h * 4);
	return SUCCEEDED(conv->CopyPixels(nullptr, w * 4, (UINT)bgra.size(), bgra.data()));
}

} // namespace hl
