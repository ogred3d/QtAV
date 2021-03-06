/******************************************************************************
    QtAV:  Media play library based on Qt and FFmpeg
    Copyright (C) 2013 Wang Bin <wbsecg1@gmail.com>

*   This file is part of QtAV

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
******************************************************************************/

#include "QtAV/Direct2DRenderer.h"
#include "private/VideoRenderer_p.h"
#include <QtGui/QPainter>
#include <QtGui/QPaintEngine>
#include <QResizeEvent>

#include <d2d1.h>

//steps: http://msdn.microsoft.com/zh-cn/library/dd317121(v=vs.85).aspx
//performance: http://msdn.microsoft.com/en-us/library/windows/desktop/dd372260(v=vs.85).aspx
//vlc is helpful

namespace QtAV {

template<class Interface>
inline void SafeRelease(Interface **ppInterfaceToRelease)
{
    if (*ppInterfaceToRelease != NULL){
        (*ppInterfaceToRelease)->Release();
        (*ppInterfaceToRelease) = NULL;
    }
}

class Direct2DRendererPrivate : public VideoRendererPrivate
{
public:
    DPTR_DECLARE_PUBLIC(Direct2DRenderer)

    Direct2DRendererPrivate():
        d2d_factory(0)
      , render_target(0)
      , bitmap(0)
      , bitmap_width(0)
      , bitmap_height(0)
    {
        HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d_factory);
        if (FAILED(hr)) {
            qWarning("Create d2d factory failed");
        }
        pixel_format = D2D1::PixelFormat(
                    DXGI_FORMAT_B8G8R8A8_UNORM,
                    D2D1_ALPHA_MODE_IGNORE
                    );
        bitmap_properties = D2D1::BitmapProperties(pixel_format);
    }
    ~Direct2DRendererPrivate() {
        SafeRelease(&d2d_factory);
        SafeRelease(&render_target);
        SafeRelease(&bitmap);
    }
    bool createDeviceResource() {
        DPTR_P(Direct2DRenderer);
        update_background = true;
        SafeRelease(&render_target); //force create a new one
        //
        //  This method creates resources which are bound to a particular
        //  Direct3D device. It's all centralized here, in case the resources
        //  need to be recreated in case of Direct3D device loss (eg. display
        //  change, remoting, removal of video card, etc).
        //
        //TODO: move to prepare(), or private. how to call less times
        D2D1_SIZE_U size = D2D1::SizeU(p.width(), p.height());//d.renderer_width, d.renderer_height?
        // Create a Direct2D render target.
        HRESULT hr = d2d_factory->CreateHwndRenderTarget(
                    D2D1::RenderTargetProperties(), //TODO: vlc set properties
                    D2D1::HwndRenderTargetProperties(p.winId(), size),
                    &render_target
                    );
        if (FAILED(hr)) {
            qWarning("CreateHwndRenderTarget() failed: %d", GetLastError());
            render_target = 0;
            return false;
        }
        SafeRelease(&bitmap);
        prepareBitmap(src_width, src_height); //bitmap depends on render target
        return hr == S_OK;
    }
    //create an empty bitmap with given size. if size is equal as current and bitmap already exists, do nothing
    bool prepareBitmap(int w, int h) {
        if (w == bitmap_width && h == bitmap_height && bitmap)
            return true;
        if (!render_target) {
            qWarning("No render target, bitmap will not be created!!!");
            return false;
        }
        bitmap_width = w;
        bitmap_height = h;
        qDebug("Resize bitmap to %d x %d", w, h);
        SafeRelease(&bitmap);
        HRESULT hr = render_target->CreateBitmap(D2D1::SizeU(w, h)
                                                   , NULL
                                                   , 0
                                                   , &bitmap_properties
                                                   , &bitmap);
        if (FAILED(hr)) {
            qWarning("Failed to create ID2D1Bitmap (%#x)", hr);
            bitmap = 0;
            SafeRelease(&render_target);
            return false;
        }
        return true;
    }

    ID2D1Factory *d2d_factory;
    ID2D1HwndRenderTarget *render_target;
    D2D1_PIXEL_FORMAT pixel_format;
    D2D1_BITMAP_PROPERTIES bitmap_properties;
    ID2D1Bitmap *bitmap;
    int bitmap_width, bitmap_height; //can not use src_width, src height because bitmap not update when they changes
};

Direct2DRenderer::Direct2DRenderer(QWidget *parent, Qt::WindowFlags f):
    QWidget(parent, f),VideoRenderer(*new Direct2DRendererPrivate())
{
    DPTR_INIT_PRIVATE(Direct2DRenderer);
    d_func().widget_holder = this;
    setAcceptDrops(true);
    setFocusPolicy(Qt::StrongFocus);
    //setAttribute(Qt::WA_OpaquePaintEvent);
    //setAttribute(Qt::WA_NoSystemBackground);
    setAutoFillBackground(false);
    setAttribute(Qt::WA_PaintOnScreen, true);
}

Direct2DRenderer::~Direct2DRenderer()
{
}

void Direct2DRenderer::convertData(const QByteArray &data)
{
    DPTR_D(Direct2DRenderer);
    if (!d.prepareBitmap(d.src_width, d.src_height))
        return;
    HRESULT hr = S_OK;
    QMutexLocker locker(&d.img_mutex);
    Q_UNUSED(locker);
    //TODO: if CopyFromMemory() is deep copy, mutex can be avoided
        /*if lock is required, do not use locker in if() scope, it will unlock outside the scope*/
        //d.img_mutex.lock();//TODO: d2d often crash, should we always lock? How about other renderer?
    hr = d.bitmap->CopyFromMemory(NULL //&D2D1::RectU(0, 0, image.width(), image.height()) /*&dstRect, NULL?*/,
                                  , data.constData()
                                  , d.src_width*4*sizeof(char));
    if (hr != S_OK) {
        qWarning("Failed to copy from memory to bitmap (%#x)", hr);
        //forgot unlock before, so use locker for easy
        return;
    }
}

QPaintEngine* Direct2DRenderer::paintEngine() const
{
    return 0; //use native engine
}

void Direct2DRenderer::paintEvent(QPaintEvent *)
{
    DPTR_D(Direct2DRenderer);
    QMutexLocker locker(&d.img_mutex);
    Q_UNUSED(locker);
    if (!d.render_target) {
        qWarning("No render target!!!");
        return;
    }
    HRESULT hr = S_OK;
    //begin paint
    //http://www.daimakuai.net/?page_id=1574
    d.render_target->BeginDraw();
    d.render_target->SetTransform(D2D1::Matrix3x2F::Identity());
    //The first bitmap size is 0x0, we should only draw the background

    if ((d.update_background && d.out_rect != rect())|| d.data.isEmpty()) {
        d.update_background = false;
        d.render_target->Clear(D2D1::ColorF(D2D1::ColorF::Black));
//http://msdn.microsoft.com/en-us/library/windows/desktop/dd535473(v=vs.85).aspx
        //ID2D1SolidColorBrush *brush;
        //d.render_target->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), &brush);
        //d.render_target->FillRectangle(D2D1::RectF(0, 0, width(), height()), brush);
    }
    if (d.data.isEmpty()) {
        //return; //why the background is whit if return? the below code draw an empty bitmap?
    }

    D2D1_RECT_F out_rect = {
        d.out_rect.left(),
        d.out_rect.top(),
        d.out_rect.right(),
        d.out_rect.bottom()
    };
    //d.render_target->SetTransform
    d.render_target->DrawBitmap(d.bitmap
                                , &out_rect
                                , 1 //opacity
                                , D2D1_BITMAP_INTERPOLATION_MODE_LINEAR
                                , &D2D1::RectF(0, 0, d.src_width, d.src_height));
    hr = d.render_target->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        qDebug("D2DERR_RECREATE_TARGET");
        hr = S_OK;
        SafeRelease(&d.render_target);
        d.createDeviceResource(); //?
    }
    //end paint
}

void Direct2DRenderer::resizeEvent(QResizeEvent *e)
{
    resizeRenderer(e->size());

    DPTR_D(Direct2DRenderer);
    d.update_background = true;
    if (d.render_target) {
        D2D1_SIZE_U size = {
            e->size().width(),
            e->size().height()
        };
        // Note: This method can fail, but it's okay to ignore the
        // error here -- it will be repeated on the next call to
        // EndDraw.
        d.render_target->Resize(size);
    }
    update();
}

void Direct2DRenderer::showEvent(QShowEvent *)
{
    DPTR_D(Direct2DRenderer);
    d.update_background = true;
    d.createDeviceResource();
}

bool Direct2DRenderer::write()
{
    update();
    return true;
}

} //namespace QtAV
