// Copyright 2016 Cheng Zhao. All rights reserved.
// Use of this source code is governed by the license that can be found in the
// LICENSE file.

#include "nativeui/win/window_win.h"

#include <memory>

#include "nativeui/gfx/geometry/rect_conversions.h"
#include "nativeui/gfx/win/double_buffer.h"
#include "nativeui/gfx/win/painter_win.h"
#include "nativeui/win/subwin_view.h"
#include "nativeui/win/util/hwnd_util.h"

namespace nu {

namespace {

// Convert between window and client areas.
Rect ContentToWindowBounds(WindowImpl* window, const Rect& bounds) {
  RECT rect = bounds.ToRECT();
  AdjustWindowRectEx(&rect, window->window_style(),
                     FALSE, window->window_ex_style());
  return Rect(rect);
}

bool IsShiftPressed() {
  return (::GetKeyState(VK_SHIFT) & 0x8000) == 0x8000;
}

}  // namespace

void TopLevelWindow::SetPixelBounds(const Rect& bounds) {
  SetWindowPos(hwnd(), NULL,
               bounds.x(), bounds.y(), bounds.width(), bounds.height(),
               SWP_NOACTIVATE | SWP_NOZORDER);
}

Rect TopLevelWindow::GetPixelBounds() {
  RECT r;
  GetWindowRect(hwnd(), &r);
  return Rect(r);
}

Rect TopLevelWindow::GetContentPixelBounds() {
  RECT r;
  GetClientRect(hwnd(), &r);
  POINT point = { r.left, r.top };
  ClientToScreen(hwnd(), &point);
  return Rect(point.x, point.y, r.right - r.left, r.bottom - r.top);
}

void TopLevelWindow::SetCapture(BaseView* view) {
  capture_view_ = view;
  ::SetCapture(hwnd());
}

void TopLevelWindow::ReleaseCapture() {
  if (::GetCapture() == hwnd())
    ::ReleaseCapture();
}

void TopLevelWindow::SetBackgroundColor(nu::Color color) {
  background_color_ = color;
  InvalidateRect(hwnd(), NULL, TRUE);
}

void TopLevelWindow::OnCaptureChanged(HWND window) {
  if (capture_view_) {
    capture_view_->OnCaptureLost();
    capture_view_ = nullptr;
  }
}

void TopLevelWindow::OnClose() {
  if (delegate_->should_close.is_null() || delegate_->should_close.Run()) {
    delegate_->on_close.Emit();
    SetMsgHandled(FALSE);
  }
}

void TopLevelWindow::OnCommand(UINT code, int command, HWND window) {
  if (::GetParent(window) != hwnd()) {
    LOG(ERROR) << "Received notification " << code << " " << command
               << "from a non-child window";
    return;
  }

  auto* control = reinterpret_cast<SubwinView*>(GetWindowUserData(window));
  control->OnCommand(code, command);
}

HBRUSH TopLevelWindow::OnCtlColorStatic(HDC dc, HWND window) {
  auto* control = reinterpret_cast<SubwinView*>(GetWindowUserData(window));
  HBRUSH brush = NULL;
  SetMsgHandled(control->OnCtlColor(dc, &brush));
  return brush;
}

void TopLevelWindow::OnSize(UINT param, const Size& size) {
  if (!delegate_->GetContentView())
    return;
  delegate_->GetContentView()->view()->SizeAllocate(Rect(size));
  RedrawWindow(hwnd(), NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN);
}

void TopLevelWindow::OnMouseMove(UINT flags, const Point& point) {
  if (!mouse_in_window_) {
    mouse_in_window_ = true;
    delegate_->GetContentView()->view()->OnMouseEnter();
    TrackMouse(true);
  }
  if (capture_view_) {
    capture_view_->OnMouseMove(flags, point);
    return;
  }
  delegate_->GetContentView()->view()->OnMouseMove(flags, point);
}

void TopLevelWindow::OnMouseLeave() {
  TrackMouse(false);
  mouse_in_window_ = false;
  delegate_->GetContentView()->view()->OnMouseLeave();
}

BOOL TopLevelWindow::OnMouseWheel(bool vertical, UINT flags, int delta,
                                  const Point& point) {
  POINT p = point.ToPOINT();
  ScreenToClient(hwnd(), &p);
  return delegate_->GetContentView()->view()->OnMouseWheel(
      vertical, flags, delta, Point(p));
}

LRESULT TopLevelWindow::OnMouseClick(UINT message, WPARAM w_param,
                                     LPARAM l_param) {
  delegate_->GetContentView()->view()->OnMouseClick(
      message, static_cast<UINT>(w_param),
      nu::Point(CR_GET_X_LPARAM(l_param), CR_GET_Y_LPARAM(l_param)));

  // Release the capture on mouse up.
  if (message == WM_LBUTTONUP)
    ReleaseCapture();
  return 0;
}

void TopLevelWindow::OnChar(UINT ch, UINT repeat, UINT flags) {
  if (ch == VK_TAB)
    focus_manager_.AdvanceFocus(delegate_->GetContentView(), IsShiftPressed());
}

void TopLevelWindow::OnPaint(HDC) {
  PAINTSTRUCT ps;
  BeginPaint(hwnd(), &ps);

  Rect bounds(GetContentPixelBounds());
  Rect dirty(ps.rcPaint);
  base::win::ScopedGetDC dc(hwnd());
  {
    // Double buffering the drawing.
    DoubleBuffer buffer(dc, bounds.size(), dirty, dirty.origin());

    // Background.
    PainterWin painter(buffer.dc(), scale_factor());
    painter.FillPixelRect(RectF(dirty), background_color_);

    // Draw.
    delegate_->GetContentView()->view()->Draw(&painter, dirty);
  }

  EndPaint(hwnd(), &ps);
}

LRESULT TopLevelWindow::OnEraseBkgnd(HDC dc) {
  // Needed to prevent resize flicker.
  return 1;
}

void TopLevelWindow::TrackMouse(bool enable) {
  TRACKMOUSEEVENT event = {0};
  event.cbSize = sizeof(event);
  event.hwndTrack = hwnd();
  event.dwFlags = (enable ? 0 : TME_CANCEL) | TME_LEAVE;
  event.dwHoverTime = 0;
  TrackMouseEvent(&event);
}

///////////////////////////////////////////////////////////////////////////////
// Public Window API implementation.

Window::~Window() {
  delete window_;
}

void Window::PlatformInit(const Options& options) {
  TopLevelWindow* win = new TopLevelWindow(this);
  window_ = win;

  if (!options.bounds.IsEmpty())
    SetBounds(options.bounds);
}

void Window::Close() {
  ::SendMessage(window_->hwnd(), WM_CLOSE, 0, 0);
}

void Window::PlatformSetContentView(Container* container) {
  container->view()->BecomeContentView(window_);
  container->Layout();
}

void Window::SetContentBounds(const RectF& bounds) {
  TopLevelWindow* win = static_cast<TopLevelWindow*>(window_);
  Rect pixel_bounds(ToEnclosingRect(ScaleRect(bounds, win->scale_factor())));
  win->SetPixelBounds(ContentToWindowBounds(win, pixel_bounds));
}

RectF Window::GetContentBounds() const {
  TopLevelWindow* win = static_cast<TopLevelWindow*>(window_);
  return ScaleRect(RectF(win->GetContentPixelBounds()),
                   1.0f / win->scale_factor());
}

void Window::SetBounds(const RectF& bounds) {
  TopLevelWindow* win = static_cast<TopLevelWindow*>(window_);
  win->SetPixelBounds(ToEnclosingRect(ScaleRect(bounds, win->scale_factor())));
}

RectF Window::GetBounds() const {
  TopLevelWindow* win = static_cast<TopLevelWindow*>(window_);
  return ScaleRect(RectF(win->GetPixelBounds()), 1.0f / win->scale_factor());
}

void Window::SetVisible(bool visible) {
  ShowWindow(window_->hwnd(), visible ? SW_SHOWNOACTIVATE : SW_HIDE);
}

bool Window::IsVisible() const {
  return !!::IsWindowVisible(window_->hwnd());
}

void Window::SetBackgroundColor(Color color) {
  TopLevelWindow* win = static_cast<TopLevelWindow*>(window_);
  win->SetBackgroundColor(color);
}

}  // namespace nu
