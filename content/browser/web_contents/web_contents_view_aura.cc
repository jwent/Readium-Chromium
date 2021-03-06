// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/web_contents/web_contents_view_aura.h"

#include "base/utf_string_conversions.h"
#include "content/browser/renderer_host/dip_util.h"
#include "content/browser/renderer_host/render_view_host_factory.h"
#include "content/browser/web_contents/interstitial_page_impl.h"
#include "content/browser/web_contents/web_contents_impl.h"
#include "content/public/browser/notification_observer.h"
#include "content/public/browser/notification_registrar.h"
#include "content/public/browser/notification_source.h"
#include "content/public/browser/notification_types.h"
#include "content/public/browser/render_view_host.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents_delegate.h"
#include "content/public/browser/web_contents_view_delegate.h"
#include "content/public/browser/web_drag_dest_delegate.h"
#include "third_party/WebKit/Source/WebKit/chromium/public/WebInputEvent.h"
#include "ui/aura/client/aura_constants.h"
#include "ui/aura/client/drag_drop_client.h"
#include "ui/aura/client/drag_drop_delegate.h"
#include "ui/aura/root_window.h"
#include "ui/aura/window.h"
#include "ui/base/clipboard/custom_data_helper.h"
#include "ui/base/dragdrop/drag_drop_types.h"
#include "ui/base/dragdrop/os_exchange_data.h"
#include "ui/base/dragdrop/os_exchange_data_provider_aura.h"
#include "ui/base/events/event.h"
#include "ui/base/hit_test.h"
#include "ui/compositor/layer.h"
#include "ui/gfx/screen.h"
#include "webkit/glue/webdropdata.h"

namespace content {
WebContentsView* CreateWebContentsView(
    WebContentsImpl* web_contents,
    WebContentsViewDelegate* delegate,
    RenderViewHostDelegateView** render_view_host_delegate_view) {
  WebContentsViewAura* rv = new WebContentsViewAura(web_contents, delegate);
  *render_view_host_delegate_view = rv;
  return rv;
}

namespace {

// Listens to all mouse drag events during a drag and drop and sends them to
// the renderer.
class WebDragSourceAura : public MessageLoopForUI::Observer,
                          public NotificationObserver {
 public:
  WebDragSourceAura(aura::Window* window, WebContentsImpl* contents)
      : window_(window),
        contents_(contents) {
    MessageLoopForUI::current()->AddObserver(this);
    registrar_.Add(this, NOTIFICATION_WEB_CONTENTS_DISCONNECTED,
                   Source<WebContents>(contents));
  }

  virtual ~WebDragSourceAura() {
    MessageLoopForUI::current()->RemoveObserver(this);
  }

  // MessageLoop::Observer implementation:
  virtual base::EventStatus WillProcessEvent(
      const base::NativeEvent& event) OVERRIDE {
    return base::EVENT_CONTINUE;
  }
  virtual void DidProcessEvent(const base::NativeEvent& event) OVERRIDE {
    if (!contents_)
      return;
    ui::EventType type = ui::EventTypeFromNative(event);
    RenderViewHost* rvh = NULL;
    switch (type) {
      case ui::ET_MOUSE_DRAGGED:
        rvh = contents_->GetRenderViewHost();
        if (rvh) {
          gfx::Point screen_loc_in_pixel = ui::EventLocationFromNative(event);
          gfx::Point screen_loc = ConvertPointToDIP(rvh->GetView(),
              screen_loc_in_pixel);
          gfx::Point client_loc = screen_loc;
          aura::Window* window = rvh->GetView()->GetNativeView();
          aura::Window::ConvertPointToTarget(window->GetRootWindow(),
              window, &client_loc);
          rvh->DragSourceMovedTo(client_loc.x(), client_loc.y(),
              screen_loc.x(), screen_loc.y());
        }
        break;
      default:
        break;
    }
  }

  virtual void Observe(int type,
      const NotificationSource& source,
      const NotificationDetails& details) OVERRIDE {
    if (type != NOTIFICATION_WEB_CONTENTS_DISCONNECTED)
      return;

    // Cancel the drag if it is still in progress.
    aura::client::DragDropClient* dnd_client =
        aura::client::GetDragDropClient(window_->GetRootWindow());
    if (dnd_client && dnd_client->IsDragDropInProgress())
      dnd_client->DragCancel();

    window_ = NULL;
    contents_ = NULL;
  }

  aura::Window* window() const { return window_; }

 private:
  aura::Window* window_;
  WebContentsImpl* contents_;
  NotificationRegistrar registrar_;

  DISALLOW_COPY_AND_ASSIGN(WebDragSourceAura);
};

// Utility to fill a ui::OSExchangeDataProviderAura object from WebDropData.
void PrepareDragData(const WebDropData& drop_data,
                     ui::OSExchangeDataProviderAura* provider) {
  if (!drop_data.text.string().empty())
    provider->SetString(drop_data.text.string());
  if (drop_data.url.is_valid())
    provider->SetURL(drop_data.url, drop_data.url_title);
  if (!drop_data.html.string().empty())
    provider->SetHtml(drop_data.html.string(), drop_data.html_base_url);
  if (!drop_data.filenames.empty()) {
    std::vector<ui::OSExchangeData::FileInfo> filenames;
    for (std::vector<WebDropData::FileInfo>::const_iterator it =
             drop_data.filenames.begin();
         it != drop_data.filenames.end(); ++it) {
      filenames.push_back(
          ui::OSExchangeData::FileInfo(
              FilePath::FromUTF8Unsafe(UTF16ToUTF8(it->path)),
              FilePath::FromUTF8Unsafe(UTF16ToUTF8(it->display_name))));
    }
    provider->SetFilenames(filenames);
  }
  if (!drop_data.custom_data.empty()) {
    Pickle pickle;
    ui::WriteCustomDataToPickle(drop_data.custom_data, &pickle);
    provider->SetPickledData(ui::Clipboard::GetWebCustomDataFormatType(),
                             pickle);
  }
}

// Utility to fill a WebDropData object from ui::OSExchangeData.
void PrepareWebDropData(WebDropData* drop_data,
                        const ui::OSExchangeData& data) {
  string16 plain_text;
  data.GetString(&plain_text);
  if (!plain_text.empty())
    drop_data->text = NullableString16(plain_text, false);

  GURL url;
  string16 url_title;
  data.GetURLAndTitle(&url, &url_title);
  if (url.is_valid()) {
    drop_data->url = url;
    drop_data->url_title = url_title;
  }

  string16 html;
  GURL html_base_url;
  data.GetHtml(&html, &html_base_url);
  if (!html.empty())
    drop_data->html = NullableString16(html, false);
  if (html_base_url.is_valid())
    drop_data->html_base_url = html_base_url;

  std::vector<ui::OSExchangeData::FileInfo> files;
  if (data.GetFilenames(&files) && !files.empty()) {
    for (std::vector<ui::OSExchangeData::FileInfo>::const_iterator
             it = files.begin(); it != files.end(); ++it) {
      drop_data->filenames.push_back(
          WebDropData::FileInfo(
              UTF8ToUTF16(it->path.AsUTF8Unsafe()),
              UTF8ToUTF16(it->display_name.AsUTF8Unsafe())));
    }
  }

  Pickle pickle;
  if (data.GetPickledData(ui::Clipboard::GetWebCustomDataFormatType(),
                          &pickle))
    ui::ReadCustomDataIntoMap(pickle.data(), pickle.size(),
                              &drop_data->custom_data);
}

// Utilities to convert between WebKit::WebDragOperationsMask and
// ui::DragDropTypes.
int ConvertFromWeb(WebKit::WebDragOperationsMask ops) {
  int drag_op = ui::DragDropTypes::DRAG_NONE;
  if (ops & WebKit::WebDragOperationCopy)
    drag_op |= ui::DragDropTypes::DRAG_COPY;
  if (ops & WebKit::WebDragOperationMove)
    drag_op |= ui::DragDropTypes::DRAG_MOVE;
  if (ops & WebKit::WebDragOperationLink)
    drag_op |= ui::DragDropTypes::DRAG_LINK;
  return drag_op;
}

WebKit::WebDragOperationsMask ConvertToWeb(int drag_op) {
  int web_drag_op = WebKit::WebDragOperationNone;
  if (drag_op & ui::DragDropTypes::DRAG_COPY)
    web_drag_op |= WebKit::WebDragOperationCopy;
  if (drag_op & ui::DragDropTypes::DRAG_MOVE)
    web_drag_op |= WebKit::WebDragOperationMove;
  if (drag_op & ui::DragDropTypes::DRAG_LINK)
    web_drag_op |= WebKit::WebDragOperationLink;
  return (WebKit::WebDragOperationsMask) web_drag_op;
}

int ConvertAuraEventFlagsToWebInputEventModifiers(int aura_event_flags) {
  int web_input_event_modifiers = 0;
  if (aura_event_flags & ui::EF_SHIFT_DOWN)
    web_input_event_modifiers |= WebKit::WebInputEvent::ShiftKey;
  if (aura_event_flags & ui::EF_CONTROL_DOWN)
    web_input_event_modifiers |= WebKit::WebInputEvent::ControlKey;
  if (aura_event_flags & ui::EF_ALT_DOWN)
    web_input_event_modifiers |= WebKit::WebInputEvent::AltKey;
  if (aura_event_flags & ui::EF_COMMAND_DOWN)
    web_input_event_modifiers |= WebKit::WebInputEvent::MetaKey;
  return web_input_event_modifiers;
}

}  // namespace


////////////////////////////////////////////////////////////////////////////////
// WebContentsViewAura, public:

WebContentsViewAura::WebContentsViewAura(
    WebContentsImpl* web_contents,
    WebContentsViewDelegate* delegate)
    : web_contents_(web_contents),
      view_(NULL),
      delegate_(delegate),
      current_drag_op_(WebKit::WebDragOperationNone),
      drag_dest_delegate_(NULL),
      current_rvh_for_drag_(NULL) {
}

////////////////////////////////////////////////////////////////////////////////
// WebContentsViewAura, private:

WebContentsViewAura::~WebContentsViewAura() {
  // Window needs a valid delegate during its destructor, so we explicitly
  // delete it here.
  window_.reset();
}

void WebContentsViewAura::SizeChangedCommon(const gfx::Size& size) {
  if (web_contents_->GetInterstitialPage())
    web_contents_->GetInterstitialPage()->SetSize(size);
  RenderWidgetHostView* rwhv =
      web_contents_->GetRenderWidgetHostView();
  if (rwhv)
    rwhv->SetSize(size);
}

void WebContentsViewAura::EndDrag(WebKit::WebDragOperationsMask ops) {
  aura::RootWindow* root_window = GetNativeView()->GetRootWindow();
  gfx::Point screen_loc =
      gfx::Screen::GetScreenFor(GetNativeView())->GetCursorScreenPoint();
  gfx::Point client_loc = screen_loc;
  RenderViewHost* rvh = web_contents_->GetRenderViewHost();
  aura::Window* window = rvh->GetView()->GetNativeView();
  aura::Window::ConvertPointToTarget(root_window, window, &client_loc);
  rvh->DragSourceEndedAt(client_loc.x(), client_loc.y(), screen_loc.x(),
      screen_loc.y(), ops);
}

////////////////////////////////////////////////////////////////////////////////
// WebContentsViewAura, WebContentsView implementation:

void WebContentsViewAura::CreateView(const gfx::Size& initial_size) {
  // NOTE: we ignore |initial_size| since in some cases it's wrong (such as
  // if the bookmark bar is not shown and you create a new tab). The right
  // value is set shortly after this, so its safe to ignore.

  window_.reset(new aura::Window(this));
  window_->set_owned_by_parent(false);
  window_->SetType(aura::client::WINDOW_TYPE_CONTROL);
  window_->SetTransparent(false);
  window_->Init(ui::LAYER_NOT_DRAWN);
  window_->SetParent(NULL);
  window_->layer()->SetMasksToBounds(true);
  window_->SetName("WebContentsViewAura");

  // delegate_->GetDragDestDelegate() creates a new delegate on every call.
  // Hence, we save a reference to it locally. Similar model is used on other
  // platforms as well.
  if (delegate_.get())
    drag_dest_delegate_ = delegate_->GetDragDestDelegate();
}

RenderWidgetHostView* WebContentsViewAura::CreateViewForWidget(
    RenderWidgetHost* render_widget_host) {
  if (render_widget_host->GetView()) {
    // During testing, the view will already be set up in most cases to the
    // test view, so we don't want to clobber it with a real one. To verify that
    // this actually is happening (and somebody isn't accidentally creating the
    // view twice), we check for the RVH Factory, which will be set when we're
    // making special ones (which go along with the special views).
    DCHECK(RenderViewHostFactory::has_factory());
    return render_widget_host->GetView();
  }

  view_ = RenderWidgetHostView::CreateViewForWidget(
      render_widget_host);
  view_->InitAsChild(NULL);
  GetNativeView()->AddChild(view_->GetNativeView());
  view_->Show();

  // We listen to drag drop events in the newly created view's window.
  aura::client::SetDragDropDelegate(view_->GetNativeView(), this);
  return view_;
}

gfx::NativeView WebContentsViewAura::GetNativeView() const {
  return window_.get();
}

gfx::NativeView WebContentsViewAura::GetContentNativeView() const {
  return view_->GetNativeView();
}

gfx::NativeWindow WebContentsViewAura::GetTopLevelNativeWindow() const {
  return window_->GetToplevelWindow();
}

void WebContentsViewAura::GetContainerBounds(gfx::Rect *out) const {
  *out = window_->GetBoundsInScreen();
}

void WebContentsViewAura::SetPageTitle(const string16& title) {
  window_->set_title(title);
}

void WebContentsViewAura::OnTabCrashed(base::TerminationStatus status,
                                       int error_code) {
  view_ = NULL;
  // Set the focus to the parent because neither the view window nor this
  // window can handle key events.
  if (window_->HasFocus() && window_->parent())
    window_->parent()->Focus();
}

void WebContentsViewAura::SizeContents(const gfx::Size& size) {
  gfx::Rect bounds = window_->bounds();
  if (bounds.size() != size) {
    bounds.set_size(size);
    window_->SetBounds(bounds);
  } else {
    // Our size matches what we want but the renderers size may not match.
    // Pretend we were resized so that the renderers size is updated too.
    SizeChangedCommon(size);
  }
}

void WebContentsViewAura::RenderViewCreated(RenderViewHost* host) {
}

void WebContentsViewAura::Focus() {
  if (web_contents_->GetInterstitialPage()) {
    web_contents_->GetInterstitialPage()->Focus();
    return;
  }

  if (delegate_.get() && delegate_->Focus())
    return;

  RenderWidgetHostView* rwhv = web_contents_->GetRenderWidgetHostView();
  if (rwhv)
    rwhv->Focus();
}

void WebContentsViewAura::SetInitialFocus() {
  if (web_contents_->FocusLocationBarByDefault())
    web_contents_->SetFocusToLocationBar(false);
  else
    Focus();
}

void WebContentsViewAura::StoreFocus() {
  if (delegate_.get())
    delegate_->StoreFocus();
}

void WebContentsViewAura::RestoreFocus() {
  if (delegate_.get())
    delegate_->RestoreFocus();
}

WebDropData* WebContentsViewAura::GetDropData() const {
  return NULL;
}

bool WebContentsViewAura::IsEventTracking() const {
  return false;
}

void WebContentsViewAura::CloseTabAfterEventTracking() {
}

gfx::Rect WebContentsViewAura::GetViewBounds() const {
  return window_->GetBoundsInRootWindow();
}

////////////////////////////////////////////////////////////////////////////////
// WebContentsViewAura, RenderViewHostDelegateView implementation:

void WebContentsViewAura::ShowContextMenu(
    const ContextMenuParams& params,
    ContextMenuSourceType type) {
  if (delegate_.get())
    delegate_->ShowContextMenu(params, type);
}

void WebContentsViewAura::ShowPopupMenu(const gfx::Rect& bounds,
                                        int item_height,
                                        double item_font_size,
                                        int selected_item,
                                        const std::vector<WebMenuItem>& items,
                                        bool right_aligned,
                                        bool allow_multiple_selection) {
  // External popup menus are only used on Mac and Android.
  NOTIMPLEMENTED();
}

void WebContentsViewAura::StartDragging(
    const WebDropData& drop_data,
    WebKit::WebDragOperationsMask operations,
    const gfx::ImageSkia& image,
    const gfx::Point& image_offset) {
  aura::RootWindow* root_window = GetNativeView()->GetRootWindow();
  if (!aura::client::GetDragDropClient(root_window))
    return;

  ui::OSExchangeDataProviderAura* provider = new ui::OSExchangeDataProviderAura;
  PrepareDragData(drop_data, provider);
  if (!image.isNull()) {
    provider->set_drag_image(image);
    provider->set_drag_image_offset(image_offset);
  }
  ui::OSExchangeData data(provider);  // takes ownership of |provider|.

  scoped_ptr<WebDragSourceAura> drag_source(
      new WebDragSourceAura(GetNativeView(), web_contents_));

  // We need to enable recursive tasks on the message loop so we can get
  // updates while in the system DoDragDrop loop.
  int result_op = 0;
  {
    // TODO(sad): Avoid using GetCursorScreenPoint here, since the drag may not
    // always start from a mouse-event (e.g. a touch or gesture event could
    // initiate the drag). The location information should be carried over from
    // webkit. http://crbug.com/114754
    gfx::Point location(
        gfx::Screen::GetScreenFor(GetNativeView())->GetCursorScreenPoint());
    MessageLoop::ScopedNestableTaskAllower allow(MessageLoop::current());
    result_op = aura::client::GetDragDropClient(root_window)->StartDragAndDrop(
        data, root_window, location, ConvertFromWeb(operations));
  }

  // Bail out immediately if the contents view window is gone. Note that it is
  // not safe to access any class members after system drag-and-drop returns
  // since the class instance might be gone. The local variable |drag_source|
  // is still valid and we can check its window property that is set to NULL
  // when the contents are gone.
  if (!drag_source->window())
    return;

  EndDrag(ConvertToWeb(result_op));
  web_contents_->GetRenderViewHost()->DragSourceSystemDragEnded();
}

void WebContentsViewAura::UpdateDragCursor(WebKit::WebDragOperation operation) {
  current_drag_op_ = operation;
}

void WebContentsViewAura::GotFocus() {
  if (web_contents_->GetDelegate())
    web_contents_->GetDelegate()->WebContentsFocused(web_contents_);
}

void WebContentsViewAura::TakeFocus(bool reverse) {
  if (web_contents_->GetDelegate() &&
      !web_contents_->GetDelegate()->TakeFocus(web_contents_, reverse) &&
      delegate_.get()) {
    delegate_->TakeFocus(reverse);
  }
}

////////////////////////////////////////////////////////////////////////////////
// WebContentsViewAura, aura::WindowDelegate implementation:

gfx::Size WebContentsViewAura::GetMinimumSize() const {
  return gfx::Size();
}

void WebContentsViewAura::OnBoundsChanged(const gfx::Rect& old_bounds,
                                          const gfx::Rect& new_bounds) {
  SizeChangedCommon(new_bounds.size());
  if (delegate_.get())
    delegate_->SizeChanged(new_bounds.size());

  // Constrained web dialogs, need to be kept centered over our content area.
  for (size_t i = 0; i < window_->children().size(); i++) {
    if (window_->children()[i]->GetProperty(
            aura::client::kConstrainedWindowKey)) {
      gfx::Rect bounds = window_->children()[i]->bounds();
      bounds.set_origin(
          gfx::Point((new_bounds.width() - bounds.width()) / 2,
                     (new_bounds.height() - bounds.height()) / 2));
      window_->children()[i]->SetBounds(bounds);
    }
  }
}

void WebContentsViewAura::OnFocus(aura::Window* old_focused_window) {
}

void WebContentsViewAura::OnBlur() {
}

gfx::NativeCursor WebContentsViewAura::GetCursor(const gfx::Point& point) {
  return gfx::kNullCursor;
}

int WebContentsViewAura::GetNonClientComponent(const gfx::Point& point) const {
  return HTCLIENT;
}

bool WebContentsViewAura::ShouldDescendIntoChildForEventHandling(
    aura::Window* child,
    const gfx::Point& location) {
  return true;
}

bool WebContentsViewAura::CanFocus() {
  // Do not take the focus if |view_| is gone because neither the view window
  // nor this window can handle key events.
  return view_ != NULL;
}

void WebContentsViewAura::OnCaptureLost() {
}

void WebContentsViewAura::OnPaint(gfx::Canvas* canvas) {
}

void WebContentsViewAura::OnDeviceScaleFactorChanged(
    float device_scale_factor) {
}

void WebContentsViewAura::OnWindowDestroying() {
}

void WebContentsViewAura::OnWindowDestroyed() {
}

void WebContentsViewAura::OnWindowTargetVisibilityChanged(bool visible) {
  if (visible)
    web_contents_->WasShown();
  else
    web_contents_->WasHidden();
}

bool WebContentsViewAura::HasHitTestMask() const {
  return false;
}

void WebContentsViewAura::GetHitTestMask(gfx::Path* mask) const {
}

scoped_refptr<ui::Texture> WebContentsViewAura::CopyTexture() {
  // The layer we create doesn't have an external texture, so this should never
  // get invoked.
  NOTREACHED();
  return scoped_refptr<ui::Texture>();
}

////////////////////////////////////////////////////////////////////////////////
// WebContentsViewAura, ui::EventHandler implementation:

ui::EventResult WebContentsViewAura::OnKeyEvent(ui::KeyEvent* event) {
  return ui::ER_UNHANDLED;
}

ui::EventResult WebContentsViewAura::OnMouseEvent(ui::MouseEvent* event) {
  if (!web_contents_->GetDelegate())
    return ui::ER_UNHANDLED;

  switch (event->type()) {
    case ui::ET_MOUSE_PRESSED:
      web_contents_->GetDelegate()->ActivateContents(web_contents_);
      break;
    case ui::ET_MOUSE_MOVED:
      web_contents_->GetDelegate()->ContentsMouseEvent(
          web_contents_,
          gfx::Screen::GetScreenFor(GetNativeView())->GetCursorScreenPoint(),
          true);
      break;
    default:
      break;
  }
  return ui::ER_UNHANDLED;
}

ui::EventResult WebContentsViewAura::OnTouchEvent(ui::TouchEvent* event) {
  return ui::ER_UNHANDLED;
}

ui::EventResult WebContentsViewAura::OnGestureEvent(
    ui::GestureEvent* event) {
  return ui::ER_UNHANDLED;
}

////////////////////////////////////////////////////////////////////////////////
// WebContentsViewAura, aura::client::DragDropDelegate implementation:

void WebContentsViewAura::OnDragEntered(const ui::DropTargetEvent& event) {
  if (drag_dest_delegate_)
    drag_dest_delegate_->DragInitialize(web_contents_);

  WebDropData drop_data;
  PrepareWebDropData(&drop_data, event.data());
  WebKit::WebDragOperationsMask op = ConvertToWeb(event.source_operations());

  gfx::Point screen_pt =
      gfx::Screen::GetScreenFor(GetNativeView())->GetCursorScreenPoint();
  current_rvh_for_drag_ = web_contents_->GetRenderViewHost();
  web_contents_->GetRenderViewHost()->DragTargetDragEnter(
      drop_data, event.location(), screen_pt, op,
      ConvertAuraEventFlagsToWebInputEventModifiers(event.flags()));

  if (drag_dest_delegate_) {
    drag_dest_delegate_->OnReceiveDragData(event.data());
    drag_dest_delegate_->OnDragEnter();
  }
}

int WebContentsViewAura::OnDragUpdated(const ui::DropTargetEvent& event) {
  DCHECK(current_rvh_for_drag_);
  if (current_rvh_for_drag_ != web_contents_->GetRenderViewHost())
    OnDragEntered(event);

  WebKit::WebDragOperationsMask op = ConvertToWeb(event.source_operations());
  gfx::Point screen_pt =
      gfx::Screen::GetScreenFor(GetNativeView())->GetCursorScreenPoint();
  web_contents_->GetRenderViewHost()->DragTargetDragOver(
      event.location(), screen_pt, op,
      ConvertAuraEventFlagsToWebInputEventModifiers(event.flags()));

  if (drag_dest_delegate_)
    drag_dest_delegate_->OnDragOver();

  return ConvertFromWeb(current_drag_op_);
}

void WebContentsViewAura::OnDragExited() {
  DCHECK(current_rvh_for_drag_);
  if (current_rvh_for_drag_ != web_contents_->GetRenderViewHost())
    return;

  web_contents_->GetRenderViewHost()->DragTargetDragLeave();
  if (drag_dest_delegate_)
    drag_dest_delegate_->OnDragLeave();
}

int WebContentsViewAura::OnPerformDrop(const ui::DropTargetEvent& event) {
  DCHECK(current_rvh_for_drag_);
  if (current_rvh_for_drag_ != web_contents_->GetRenderViewHost())
    OnDragEntered(event);

  web_contents_->GetRenderViewHost()->DragTargetDrop(
      event.location(),
      gfx::Screen::GetScreenFor(GetNativeView())->GetCursorScreenPoint(),
      ConvertAuraEventFlagsToWebInputEventModifiers(event.flags()));
  if (drag_dest_delegate_)
    drag_dest_delegate_->OnDrop();
  return current_drag_op_;
}

}  // namespace content
