/*************************************************************************/
/*  display_server_xcb.cpp                                               */
/*************************************************************************/
/*                       This file is part of:                           */
/*                           GODOT ENGINE                                */
/*                      https://godotengine.org                          */
/*************************************************************************/
/* Copyright (c) 2007-2021 Juan Linietsky, Ariel Manzur.                 */
/* Copyright (c) 2014-2021 Godot Engine contributors (cf. AUTHORS.md).   */
/*                                                                       */
/* Permission is hereby granted, free of charge, to any person obtaining */
/* a copy of this software and associated documentation files (the       */
/* "Software"), to deal in the Software without restriction, including   */
/* without limitation the rights to use, copy, modify, merge, publish,   */
/* distribute, sublicense, and/or sell copies of the Software, and to    */
/* permit persons to whom the Software is furnished to do so, subject to */
/* the following conditions:                                             */
/*                                                                       */
/* The above copyright notice and this permission notice shall be        */
/* included in all copies or substantial portions of the Software.       */
/*                                                                       */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,       */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF    */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.*/
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY  */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,  */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE     */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                */
/*************************************************************************/

#include "display_server_xcb.h"

#ifdef XCB_ENABLED

#include "core/config/project_settings.h"
#include "core/string/print_string.h"
#include "detect_prime_x11.h"
#include "key_mapping_x11.h"
#include "main/main.h"
#include "scene/resources/texture.h"

#if defined(VULKAN_ENABLED)
#include "servers/rendering/renderer_rd/renderer_compositor_rd.h"
#endif

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xinerama.h>
#include <X11/extensions/shape.h>

// ICCCM
#define WM_NormalState 1L // window normal state
#define WM_IconicState 3L // window minimized
// EWMH
#define _NET_WM_STATE_REMOVE 0L // remove/unset property
#define _NET_WM_STATE_ADD 1L // add/set property
#define _NET_WM_STATE_TOGGLE 2L // toggle property

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

//stupid linux.h
#ifdef KEY_TAB
#undef KEY_TAB
#endif

#undef CursorShape
#include <X11/XKBlib.h>

// 2.2 is the first release with multitouch
#define XINPUT_CLIENT_VERSION_MAJOR 2
#define XINPUT_CLIENT_VERSION_MINOR 2

#define VALUATOR_ABSX 0
#define VALUATOR_ABSY 1
#define VALUATOR_PRESSURE 2
#define VALUATOR_TILTX 3
#define VALUATOR_TILTY 4

//#define DISPLAY_SERVER_X11_DEBUG_LOGS_ENABLED
#ifdef DISPLAY_SERVER_X11_DEBUG_LOGS_ENABLED
#define DEBUG_LOG_X11(...) printf(__VA_ARGS__)
#else
#define DEBUG_LOG_X11(...)
#endif

static const double abs_resolution_mult = 10000.0;
static const double abs_resolution_range_mult = 10.0;

// Hints for X11 fullscreen
struct Hints {
	unsigned long flags = 0;
	unsigned long functions = 0;
	unsigned long decorations = 0;
	long inputMode = 0;
	unsigned long status = 0;
};

bool DisplayServerXCB::has_feature(Feature p_feature) const {
	switch (p_feature) {
		case FEATURE_SUBWINDOWS:
#ifdef TOUCH_ENABLED
		case FEATURE_TOUCHSCREEN:
#endif
		case FEATURE_MOUSE:
		case FEATURE_MOUSE_WARP:
		case FEATURE_CLIPBOARD:
		case FEATURE_CURSOR_SHAPE:
		case FEATURE_CUSTOM_CURSOR_SHAPE:
		case FEATURE_IME:
		case FEATURE_WINDOW_TRANSPARENCY:
		//case FEATURE_HIDPI:
		case FEATURE_ICON:
		case FEATURE_NATIVE_ICON:
		case FEATURE_SWAP_BUFFERS:
			return true;
		default: {
		}
	}

	return false;
}

String DisplayServerXCB::get_name() const {
	return "XCB";
}

void DisplayServerXCB::alert(const String &p_alert, const String &p_title) {
	const char *message_programs[] = { "zenity", "kdialog", "Xdialog", "xmessage" };

	String path = OS::get_singleton()->get_environment("PATH");
	Vector<String> path_elems = path.split(":", false);
	String program;

	for (int i = 0; i < path_elems.size(); i++) {
		for (uint64_t k = 0; k < sizeof(message_programs) / sizeof(char *); k++) {
			String tested_path = path_elems[i].plus_file(message_programs[k]);

			if (FileAccess::exists(tested_path)) {
				program = tested_path;
				break;
			}
		}

		if (program.length()) {
			break;
		}
	}

	List<String> args;

	if (program.ends_with("zenity")) {
		args.push_back("--error");
		args.push_back("--width");
		args.push_back("500");
		args.push_back("--title");
		args.push_back(p_title);
		args.push_back("--text");
		args.push_back(p_alert);
	}

	if (program.ends_with("kdialog")) {
		args.push_back("--error");
		args.push_back(p_alert);
		args.push_back("--title");
		args.push_back(p_title);
	}

	if (program.ends_with("Xdialog")) {
		args.push_back("--title");
		args.push_back(p_title);
		args.push_back("--msgbox");
		args.push_back(p_alert);
		args.push_back("0");
		args.push_back("0");
	}

	if (program.ends_with("xmessage")) {
		args.push_back("-center");
		args.push_back("-title");
		args.push_back(p_title);
		args.push_back(p_alert);
	}

	if (program.length()) {
		OS::get_singleton()->execute(program, args);
	} else {
		print_line(p_alert);
	}
}

void DisplayServerXCB::_update_real_mouse_position(const WindowData &wd) {
	Window root_return, child_return;
	int root_x, root_y, win_x, win_y;
	unsigned int mask_return;

	/*
	Bool xquerypointer_result = XQueryPointer(x11_display, wd.x11_window, &root_return, &child_return, &root_x, &root_y,
			&win_x, &win_y, &mask_return);

	if (xquerypointer_result) {
		if (win_x > 0 && win_y > 0 && win_x <= wd.size.width && win_y <= wd.size.height) {
			last_mouse_pos.x = win_x;
			last_mouse_pos.y = win_y;
			last_mouse_pos_valid = true;
			Input::get_singleton()->set_mouse_position(last_mouse_pos);
		}
	}*/
}

bool DisplayServerXCB::_refresh_device_info() {
	/*int event_base, error_base;

	print_verbose("XInput: Refreshing devices.");

	
	if (!XQueryExtension(x11_display, "XInputExtension", &xi.opcode, &event_base, &error_base)) {
		print_verbose("XInput extension not available. Please upgrade your distribution.");
		return false;
	}

	int xi_major_query = XINPUT_CLIENT_VERSION_MAJOR;
	int xi_minor_query = XINPUT_CLIENT_VERSION_MINOR;

	if (XIQueryVersion(x11_display, &xi_major_query, &xi_minor_query) != Success) {
		print_verbose(vformat("XInput 2 not available (server supports %d.%d).", xi_major_query, xi_minor_query));
		xi.opcode = 0;
		return false;
	}

	if (xi_major_query < XINPUT_CLIENT_VERSION_MAJOR || (xi_major_query == XINPUT_CLIENT_VERSION_MAJOR && xi_minor_query < XINPUT_CLIENT_VERSION_MINOR)) {
		print_verbose(vformat("XInput %d.%d not available (server supports %d.%d). Touch input unavailable.",
				XINPUT_CLIENT_VERSION_MAJOR, XINPUT_CLIENT_VERSION_MINOR, xi_major_query, xi_minor_query));
	}

	xi.absolute_devices.clear();
	xi.touch_devices.clear();

	int dev_count;
	XIDeviceInfo *info = XIQueryDevice(x11_display, XIAllDevices, &dev_count);

	for (int i = 0; i < dev_count; i++) {
		XIDeviceInfo *dev = &info[i];
		if (!dev->enabled) {
			continue;
		}
		if (!(dev->use == XIMasterPointer || dev->use == XIFloatingSlave)) {
			continue;
		}

		bool direct_touch = false;
		bool absolute_mode = false;
		int resolution_x = 0;
		int resolution_y = 0;
		double abs_x_min = 0;
		double abs_x_max = 0;
		double abs_y_min = 0;
		double abs_y_max = 0;
		double pressure_min = 0;
		double pressure_max = 0;
		double tilt_x_min = 0;
		double tilt_x_max = 0;
		double tilt_y_min = 0;
		double tilt_y_max = 0;
		for (int j = 0; j < dev->num_classes; j++) {
#ifdef TOUCH_ENABLED
			if (dev->classes[j]->type == XITouchClass && ((XITouchClassInfo *)dev->classes[j])->mode == XIDirectTouch) {
				direct_touch = true;
			}
#endif
			if (dev->classes[j]->type == XIValuatorClass) {
				XIValuatorClassInfo *class_info = (XIValuatorClassInfo *)dev->classes[j];

				if (class_info->number == VALUATOR_ABSX && class_info->mode == XIModeAbsolute) {
					resolution_x = class_info->resolution;
					abs_x_min = class_info->min;
					abs_y_max = class_info->max;
					absolute_mode = true;
				} else if (class_info->number == VALUATOR_ABSY && class_info->mode == XIModeAbsolute) {
					resolution_y = class_info->resolution;
					abs_y_min = class_info->min;
					abs_y_max = class_info->max;
					absolute_mode = true;
				} else if (class_info->number == VALUATOR_PRESSURE && class_info->mode == XIModeAbsolute) {
					pressure_min = class_info->min;
					pressure_max = class_info->max;
				} else if (class_info->number == VALUATOR_TILTX && class_info->mode == XIModeAbsolute) {
					tilt_x_min = class_info->min;
					tilt_x_max = class_info->max;
				} else if (class_info->number == VALUATOR_TILTY && class_info->mode == XIModeAbsolute) {
					tilt_x_min = class_info->min;
					tilt_x_max = class_info->max;
				}
			}
		}
		if (direct_touch) {
			xi.touch_devices.push_back(dev->deviceid);
			print_verbose("XInput: Using touch device: " + String(dev->name));
		}
		if (absolute_mode) {
			// If no resolution was reported, use the min/max ranges.
			if (resolution_x <= 0) {
				resolution_x = (abs_x_max - abs_x_min) * abs_resolution_range_mult;
			}
			if (resolution_y <= 0) {
				resolution_y = (abs_y_max - abs_y_min) * abs_resolution_range_mult;
			}
			xi.absolute_devices[dev->deviceid] = Vector2(abs_resolution_mult / resolution_x, abs_resolution_mult / resolution_y);
			print_verbose("XInput: Absolute pointing device: " + String(dev->name));
		}

		xi.pressure = 0;
		xi.pen_pressure_range[dev->deviceid] = Vector2(pressure_min, pressure_max);
		xi.pen_tilt_x_range[dev->deviceid] = Vector2(tilt_x_min, tilt_x_max);
		xi.pen_tilt_y_range[dev->deviceid] = Vector2(tilt_y_min, tilt_y_max);
	}

	XIFreeDeviceInfo(info);
#ifdef TOUCH_ENABLED
	if (!xi.touch_devices.size()) {
		print_verbose("XInput: No touch devices found.");
	}
#endif
	*/
	return true;
}

void DisplayServerXCB::_flush_mouse_motion() {
	// Block events polling while flushing motion events.
	/*MutexLock mutex_lock(events_mutex);

	for (uint32_t event_index = 0; event_index < polled_events.size(); ++event_index) {
		XEvent &event = polled_events[event_index];
		if (XGetEventData(x11_display, &event.xcookie) && event.xcookie.type == GenericEvent && event.xcookie.extension == xi.opcode) {
			XIDeviceEvent *event_data = (XIDeviceEvent *)event.xcookie.data;
			if (event_data->evtype == XI_RawMotion) {
				XFreeEventData(x11_display, &event.xcookie);
				polled_events.remove(event_index--);
				continue;
			}
			XFreeEventData(x11_display, &event.xcookie);
			break;
		}
	}

	xi.relative_motion.x = 0;
	xi.relative_motion.y = 0;*/
}

void DisplayServerXCB::mouse_set_mode(MouseMode p_mode){
	_THREAD_SAFE_METHOD_

	/*if (p_mode == mouse_mode) {
		return;
	}

	if (mouse_mode == MOUSE_MODE_CAPTURED || mouse_mode == MOUSE_MODE_CONFINED) {
		XUngrabPointer(x11_display, CurrentTime);
	}

	// The only modes that show a cursor are VISIBLE and CONFINED
	bool showCursor = (p_mode == MOUSE_MODE_VISIBLE || p_mode == MOUSE_MODE_CONFINED);

	for (Map<WindowID, WindowData>::Element *E = windows.front(); E; E = E->next()) {
		if (showCursor) {
			XDefineCursor(x11_display, E->get().x11_window, cursors[current_cursor]); // show cursor
		} else {
			XDefineCursor(x11_display, E->get().x11_window, null_cursor); // hide cursor
		}
	}
	mouse_mode = p_mode;

	if (mouse_mode == MOUSE_MODE_CAPTURED || mouse_mode == MOUSE_MODE_CONFINED) {
		//flush pending motion events
		_flush_mouse_motion();
		WindowData &main_window = windows[MAIN_WINDOW_ID];

		if (XGrabPointer(
					x11_display, main_window.x11_window, True,
					ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
					GrabModeAsync, GrabModeAsync, windows[MAIN_WINDOW_ID].x11_window, None, CurrentTime) != GrabSuccess) {
			ERR_PRINT("NO GRAB");
		}

		if (mouse_mode == MOUSE_MODE_CAPTURED) {
			center.x = main_window.size.width / 2;
			center.y = main_window.size.height / 2;

			XWarpPointer(x11_display, None, main_window.x11_window,
					0, 0, 0, 0, (int)center.x, (int)center.y);

			Input::get_singleton()->set_mouse_position(center);
		}
	} else {
		do_mouse_warp = false;
	}

	XFlush(x11_display);*/
}

DisplayServerXCB::MouseMode DisplayServerXCB::mouse_get_mode() const {
	return mouse_mode;
}

void DisplayServerXCB::mouse_warp_to_position(const Point2i &p_to) {
	/*_THREAD_SAFE_METHOD_

	if (mouse_mode == MOUSE_MODE_CAPTURED) {
		last_mouse_pos = p_to;
	} else {
		XWarpPointer(x11_display, None, windows[MAIN_WINDOW_ID].x11_window,
				0, 0, 0, 0, (int)p_to.x, (int)p_to.y);
	}*/
}

Point2i DisplayServerXCB::mouse_get_position() const {
	/*int root_x, root_y;
	int win_x, win_y;
	unsigned int mask_return;
	Window window_returned;

	Bool result = XQueryPointer(x11_display, RootWindow(x11_display, DefaultScreen(x11_display)), &window_returned,
			&window_returned, &root_x, &root_y, &win_x, &win_y,
			&mask_return);
	if (result == True) {
		return Point2i(root_x, root_y);
	}*/
	return Point2i();
}

Point2i DisplayServerXCB::mouse_get_absolute_position() const {
	int number_of_screens = get_screen_count();
	/*for (int i = 0; i < number_of_screens; i++) {
		Window root, child;
		int root_x, root_y, win_x, win_y;
		unsigned int mask;
		if (XQueryPointer(x11_display, XRootWindow(x11_display, i), &root, &child, &root_x, &root_y, &win_x, &win_y, &mask)) {
			XWindowAttributes root_attrs;
			XGetWindowAttributes(x11_display, root, &root_attrs);

			return Vector2i(root_attrs.x + root_x, root_attrs.y + root_y);
		}
	}*/
	return Vector2i();
}

int DisplayServerXCB::mouse_get_button_state() const {
	return last_button_state;
}

void DisplayServerXCB::clipboard_set(const String &p_text) {
	/*_THREAD_SAFE_METHOD_

	{
		// The clipboard content can be accessed while polling for events.
		MutexLock mutex_lock(events_mutex);
		internal_clipboard = p_text;
	}

	XSetSelectionOwner(x11_display, XA_PRIMARY, windows[MAIN_WINDOW_ID].x11_window, CurrentTime);
	XSetSelectionOwner(x11_display, XInternAtom(x11_display, "CLIPBOARD", 0), windows[MAIN_WINDOW_ID].x11_window, CurrentTime);*/
}

String DisplayServerXCB::clipboard_get() const {
	_THREAD_SAFE_METHOD_

	String ret;
	/*ret = _clipboard_get(XInternAtom(x11_display, "CLIPBOARD", 0), windows[MAIN_WINDOW_ID].x11_window);

	if (ret.is_empty()) {
		ret = _clipboard_get(XA_PRIMARY, windows[MAIN_WINDOW_ID].x11_window);
	}
	*/
	return ret;
}
int DisplayServerXCB::get_screen_count() const {
	/*_THREAD_SAFE_METHOD_

	// Using Xinerama Extension
	int event_base, error_base;
	const Bool ext_okay = XineramaQueryExtension(x11_display, &event_base, &error_base);
	if (!ext_okay) {
		return 0;
	}

	int count;
	XineramaScreenInfo *xsi = XineramaQueryScreens(x11_display, &count);
	XFree(xsi);
	return count;*/
	int nscreen = xcb_setup_roots_length(xcb_get_setup(xcb_connection));
	print_line("DisplayServerXCB::get_screen_count: " + String::num_uint64(nscreen));
	return nscreen;
}

Point2i DisplayServerXCB::screen_get_position(int p_screen) const {
	/*_THREAD_SAFE_METHOD_

	if (p_screen == SCREEN_OF_MAIN_WINDOW) {
		p_screen = window_get_current_screen();
	}

	// Using Xinerama Extension
	int event_base, error_base;
	const Bool ext_okay = XineramaQueryExtension(x11_display, &event_base, &error_base);
	if (!ext_okay) {
		return Point2i(0, 0);
	}

	int count;
	XineramaScreenInfo *xsi = XineramaQueryScreens(x11_display, &count);

	// Check if screen is valid
	ERR_FAIL_INDEX_V(p_screen, count, Point2i(0, 0));

	Point2i position = Point2i(xsi[p_screen].x_org, xsi[p_screen].y_org);

	XFree(xsi);

	return position;*/
	return {};
}

Size2i DisplayServerXCB::screen_get_size(int p_screen) const {
	return screen_get_usable_rect(p_screen).size;
}

Rect2i DisplayServerXCB::screen_get_usable_rect(int p_screen) const {
	printf("DisplayServerXCB::screen_get_usable_rect %d\n", p_screen);
	_THREAD_SAFE_METHOD_
	Rect2i rect;

	xcb_randr_get_screen_resources_current_reply_t *reply = xcb_randr_get_screen_resources_current_reply(
			xcb_connection, xcb_randr_get_screen_resources_current(xcb_connection, xcb_screen->root), nullptr);

	xcb_timestamp_t timestamp = reply->config_timestamp;
	int count = xcb_randr_get_screen_resources_current_outputs_length(reply);
	xcb_randr_output_t *randr_outputs = xcb_randr_get_screen_resources_current_outputs(reply);
	printf("Count: %d\n", count);
	ERR_FAIL_INDEX_V(p_screen, count, Rect2i(0, 0, 0, 0));
	int index = 0;
	for (int i = 0; i < count; ++i) {
		xcb_randr_get_output_info_reply_t *output = xcb_randr_get_output_info_reply(
				xcb_connection, xcb_randr_get_output_info(xcb_connection, randr_outputs[i], timestamp), nullptr);
		if (output == nullptr)
			continue;

		if (output->crtc == XCB_NONE || output->connection == XCB_RANDR_CONNECTION_DISCONNECTED)
			continue;

		if (p_screen == index) {
			xcb_randr_get_crtc_info_reply_t *crtc = xcb_randr_get_crtc_info_reply(xcb_connection,
					xcb_randr_get_crtc_info(xcb_connection, output->crtc, timestamp), NULL);
			printf("index = %d | x = %d | y = %d | w = %d | h = %d\n",
					index, crtc->x, crtc->y, crtc->width, crtc->height);

			rect = Rect2i(crtc->x, crtc->y, crtc->width, crtc->height);

			free(crtc);
		}
		free(output);
		++index;
	}

	free(reply);

	return rect;

	/*_THREAD_SAFE_METHOD_

	if (p_screen == SCREEN_OF_MAIN_WINDOW) {
		p_screen = window_get_current_screen();
	}

	// Using Xinerama Extension
	int event_base, error_base;
	const Bool ext_okay = XineramaQueryExtension(x11_display, &event_base, &error_base);
	if (!ext_okay) {
		return Rect2i(0, 0, 0, 0);
	}

	int count;
	XineramaScreenInfo *xsi = XineramaQueryScreens(x11_display, &count);

	// Check if screen is valid
	ERR_FAIL_INDEX_V(p_screen, count, Rect2i(0, 0, 0, 0));

	Rect2i rect = Rect2i(xsi[p_screen].x_org, xsi[p_screen].y_org, xsi[p_screen].width, xsi[p_screen].height);
	XFree(xsi);
	return rect;*/
	return {};
}

int DisplayServerXCB::screen_get_dpi(int p_screen) const {
	_THREAD_SAFE_METHOD_

	if (p_screen == SCREEN_OF_MAIN_WINDOW) {
		p_screen = window_get_current_screen();
	}

	//invalid screen?
	ERR_FAIL_INDEX_V(p_screen, get_screen_count(), 0);

	//Get physical monitor Dimensions through XRandR and calculate dpi
	Size2i sc = screen_get_size(p_screen);

	int width_mm = xcb_screen->width_in_millimeters;
	int height_mm = xcb_screen->height_in_millimeters;
	double xdpi = (width_mm ? sc.width / (double)width_mm * 25.4 : 0);
	double ydpi = (height_mm ? sc.height / (double)height_mm * 25.4 : 0);

	int dpi = 96; // default dpi
	if (xdpi || ydpi) {
		dpi = (xdpi + ydpi) / (xdpi && ydpi ? 2 : 1);
	}
	printf("DisplayServerXCB::screen_get_dpi %d\n", dpi);
	return dpi;
}

bool DisplayServerXCB::screen_is_touchscreen(int p_screen) const {
	_THREAD_SAFE_METHOD_

#ifndef _MSC_VER
#warning Need to get from proper window
#endif

	return DisplayServer::screen_is_touchscreen(p_screen);
}

Vector<DisplayServer::WindowID> DisplayServerXCB::get_window_list() const {
	_THREAD_SAFE_METHOD_

	Vector<int> ret;
	for (Map<WindowID, WindowData>::Element *E = windows.front(); E; E = E->next()) {
		ret.push_back(E->key());
	}
	return ret;
}

DisplayServer::WindowID DisplayServerXCB::create_sub_window(WindowMode p_mode, uint32_t p_flags, const Rect2i &p_rect) {
	print_line("DisplayServerXCB::create_sub_window");

	WindowID id = _create_window(p_mode, p_flags, p_rect);
	for (int i = 0; i < WINDOW_FLAG_MAX; i++) {
		if (p_flags & (1 << i)) {
			window_set_flag(WindowFlags(i), true, id);
		}
	}

	return id;
}

void DisplayServerXCB::show_window(WindowID p_id) {
	WindowData &wd = windows[p_id];
	print_line("DisplayServerXCB::show_window " + String::num_uint64(wd.xcb_window));
	xcb_map_window(xcb_connection, wd.xcb_window);

	/*_THREAD_SAFE_METHOD_


	XMapWindow(x11_display, wd.x11_window);*/
}

void DisplayServerXCB::delete_sub_window(WindowID p_id) {
	/*_THREAD_SAFE_METHOD_

	ERR_FAIL_COND(!windows.has(p_id));
	ERR_FAIL_COND_MSG(p_id == MAIN_WINDOW_ID, "Main window can't be deleted"); //ma

	WindowData &wd = windows[p_id];

	DEBUG_LOG_X11("delete_sub_window: %lu (%u) \n", wd.x11_window, p_id);

	while (wd.transient_children.size()) {
		window_set_transient(wd.transient_children.front()->get(), INVALID_WINDOW_ID);
	}

	if (wd.transient_parent != INVALID_WINDOW_ID) {
		window_set_transient(p_id, INVALID_WINDOW_ID);
	}

#ifdef VULKAN_ENABLED
	if (rendering_driver == "vulkan") {
		context_vulkan->window_destroy(p_id);
	}
#endif
	XUnmapWindow(x11_display, wd.x11_window);
	XDestroyWindow(x11_display, wd.x11_window);
	if (wd.xic) {
		XDestroyIC(wd.xic);
		wd.xic = nullptr;
	}

	windows.erase(p_id);*/
}

void DisplayServerXCB::window_attach_instance_id(ObjectID p_instance, WindowID p_window) {
	/*ERR_FAIL_COND(!windows.has(p_window));
	WindowData &wd = windows[p_window];

	wd.instance_id = p_instance;*/
}

ObjectID DisplayServerXCB::window_get_attached_instance_id(WindowID p_window) const {
	ERR_FAIL_COND_V(!windows.has(p_window), ObjectID());
	const WindowData &wd = windows[p_window];
	return wd.instance_id;
}

DisplayServerXCB::WindowID DisplayServerXCB::get_window_at_screen_position(const Point2i &p_position) const {
	/*WindowID found_window = INVALID_WINDOW_ID;
	WindowID parent_window = INVALID_WINDOW_ID;
	unsigned int focus_order = 0;
	for (Map<WindowID, WindowData>::Element *E = windows.front(); E; E = E->next()) {
		const WindowData &wd = E->get();

		// Discard windows with no focus.
		if (wd.focus_order == 0) {
			continue;
		}

		// Find topmost window which contains the given position.
		WindowID window_id = E->key();
		Rect2i win_rect = Rect2i(window_get_position(window_id), window_get_size(window_id));
		if (win_rect.has_point(p_position)) {
			// For siblings, pick the window which was focused last.
			if ((parent_window != wd.transient_parent) || (wd.focus_order > focus_order)) {
				found_window = window_id;
				parent_window = wd.transient_parent;
				focus_order = wd.focus_order;
			}
		}
	}

	return found_window;*/
	return INVALID_WINDOW_ID;
}

void DisplayServerXCB::window_set_title(const String &p_title, WindowID p_window) {
	/*_THREAD_SAFE_METHOD_

	ERR_FAIL_COND(!windows.has(p_window));
	WindowData &wd = windows[p_window];

	XStoreName(x11_display, wd.x11_window, p_title.utf8().get_data());

	Atom _net_wm_name = XInternAtom(x11_display, "_NET_WM_NAME", false);
	Atom utf8_string = XInternAtom(x11_display, "UTF8_STRING", false);
	if (_net_wm_name != None && utf8_string != None) {
		XChangeProperty(x11_display, wd.x11_window, _net_wm_name, utf8_string, 8, PropModeReplace, (unsigned char *)p_title.utf8().get_data(), p_title.utf8().length());
	}*/
}

void DisplayServerXCB::window_set_mouse_passthrough(const Vector<Vector2> &p_region, WindowID p_window) {
	/*_THREAD_SAFE_METHOD_

	ERR_FAIL_COND(!windows.has(p_window));
	const WindowData &wd = windows[p_window];

	int event_base, error_base;
	const Bool ext_okay = XShapeQueryExtension(x11_display, &event_base, &error_base);
	if (ext_okay) {
		Region region;
		if (p_region.size() == 0) {
			region = XCreateRegion();
			XRectangle rect;
			rect.x = 0;
			rect.y = 0;
			rect.width = window_get_real_size(p_window).x;
			rect.height = window_get_real_size(p_window).y;
			XUnionRectWithRegion(&rect, region, region);
		} else {
			XPoint *points = (XPoint *)memalloc(sizeof(XPoint) * p_region.size());
			for (int i = 0; i < p_region.size(); i++) {
				points[i].x = p_region[i].x;
				points[i].y = p_region[i].y;
			}
			region = XPolygonRegion(points, p_region.size(), EvenOddRule);
			memfree(points);
		}
		XShapeCombineRegion(x11_display, wd.x11_window, ShapeInput, 0, 0, region, ShapeSet);
		XDestroyRegion(region);
	}*/
}

void DisplayServerXCB::window_set_rect_changed_callback(const Callable &p_callable, WindowID p_window) {
	_THREAD_SAFE_METHOD_

	ERR_FAIL_COND(!windows.has(p_window));
	WindowData &wd = windows[p_window];
	wd.rect_changed_callback = p_callable;
}

void DisplayServerXCB::window_set_window_event_callback(const Callable &p_callable, WindowID p_window) {
	_THREAD_SAFE_METHOD_

	ERR_FAIL_COND(!windows.has(p_window));
	WindowData &wd = windows[p_window];
	wd.event_callback = p_callable;
}

void DisplayServerXCB::window_set_input_event_callback(const Callable &p_callable, WindowID p_window) {
	_THREAD_SAFE_METHOD_

	ERR_FAIL_COND(!windows.has(p_window));
	WindowData &wd = windows[p_window];
	wd.input_event_callback = p_callable;
}

void DisplayServerXCB::window_set_input_text_callback(const Callable &p_callable, WindowID p_window) {
	_THREAD_SAFE_METHOD_

	ERR_FAIL_COND(!windows.has(p_window));
	WindowData &wd = windows[p_window];
	wd.input_text_callback = p_callable;
}

void DisplayServerXCB::window_set_drop_files_callback(const Callable &p_callable, WindowID p_window) {
	_THREAD_SAFE_METHOD_

	ERR_FAIL_COND(!windows.has(p_window));
	WindowData &wd = windows[p_window];
	wd.drop_files_callback = p_callable;
}

int DisplayServerXCB::window_get_current_screen(WindowID p_window) const {
	/*_THREAD_SAFE_METHOD_

	ERR_FAIL_COND_V(!windows.has(p_window), -1);
	const WindowData &wd = windows[p_window];

	int x, y;
	Window child;
	XTranslateCoordinates(x11_display, wd.x11_window, DefaultRootWindow(x11_display), 0, 0, &x, &y, &child);

	int count = get_screen_count();
	for (int i = 0; i < count; i++) {
		Point2i pos = screen_get_position(i);
		Size2i size = screen_get_size(i);
		if ((x >= pos.x && x < pos.x + size.width) && (y >= pos.y && y < pos.y + size.height)) {
			return i;
		}
	}*/
	return 0;
}

void DisplayServerXCB::window_set_current_screen(int p_screen, WindowID p_window) {
	/*_THREAD_SAFE_METHOD_

	ERR_FAIL_COND(!windows.has(p_window));
	WindowData &wd = windows[p_window];

	if (p_screen == SCREEN_OF_MAIN_WINDOW) {
		p_screen = window_get_current_screen();
	}

	// Check if screen is valid
	ERR_FAIL_INDEX(p_screen, get_screen_count());

	if (window_get_mode(p_window) == WINDOW_MODE_FULLSCREEN) {
		Point2i position = screen_get_position(p_screen);
		Size2i size = screen_get_size(p_screen);

		XMoveResizeWindow(x11_display, wd.x11_window, position.x, position.y, size.x, size.y);
	} else {
		if (p_screen != window_get_current_screen(p_window)) {
			Point2i position = screen_get_position(p_screen);
			XMoveWindow(x11_display, wd.x11_window, position.x, position.y);
		}
	}*/
}

void DisplayServerXCB::window_set_transient(WindowID p_window, WindowID p_parent) {
	/*_THREAD_SAFE_METHOD_

	ERR_FAIL_COND(p_window == p_parent);

	ERR_FAIL_COND(!windows.has(p_window));
	WindowData &wd_window = windows[p_window];

	WindowID prev_parent = wd_window.transient_parent;
	ERR_FAIL_COND(prev_parent == p_parent);

	ERR_FAIL_COND_MSG(wd_window.on_top, "Windows with the 'on top' can't become transient.");
	if (p_parent == INVALID_WINDOW_ID) {
		//remove transient

		ERR_FAIL_COND(prev_parent == INVALID_WINDOW_ID);
		ERR_FAIL_COND(!windows.has(prev_parent));

		WindowData &wd_parent = windows[prev_parent];

		wd_window.transient_parent = INVALID_WINDOW_ID;
		wd_parent.transient_children.erase(p_window);

		XSetTransientForHint(x11_display, wd_window.x11_window, None);

		// Set focus to parent sub window to avoid losing all focus with nested menus.
		// RevertToPointerRoot is used to make sure we don't lose all focus in case
		// a subwindow and its parent are both destroyed.
		if (wd_window.menu_type && !wd_window.no_focus) {
			if (!wd_parent.no_focus) {
				XSetInputFocus(x11_display, wd_parent.x11_window, RevertToPointerRoot, CurrentTime);
			}
		}
	} else {
		ERR_FAIL_COND(!windows.has(p_parent));
		ERR_FAIL_COND_MSG(prev_parent != INVALID_WINDOW_ID, "Window already has a transient parent");
		WindowData &wd_parent = windows[p_parent];

		wd_window.transient_parent = p_parent;
		wd_parent.transient_children.insert(p_window);

		XSetTransientForHint(x11_display, wd_window.x11_window, wd_parent.x11_window);
	}*/
}

// Helper method. Assumes that the window id has already been checked and exists.
void DisplayServerXCB::_update_size_hints(WindowID p_window) {
	/*WindowData &wd = windows[p_window];
	WindowMode window_mode = window_get_mode(p_window);
	XSizeHints *xsh = XAllocSizeHints();

	// Always set the position and size hints - they should be synchronized with the actual values after the window is mapped anyway
	xsh->flags |= PPosition | PSize;
	xsh->x = wd.position.x;
	xsh->y = wd.position.y;
	xsh->width = wd.size.width;
	xsh->height = wd.size.height;

	if (window_mode == WINDOW_MODE_FULLSCREEN) {
		// Do not set any other hints to prevent the window manager from ignoring the fullscreen flags
	} else if (window_get_flag(WINDOW_FLAG_RESIZE_DISABLED, p_window)) {
		// If resizing is disabled, use the forced size
		xsh->flags |= PMinSize | PMaxSize;
		xsh->min_width = wd.size.x;
		xsh->max_width = wd.size.x;
		xsh->min_height = wd.size.y;
		xsh->max_height = wd.size.y;
	} else {
		// Otherwise, just respect min_size and max_size
		if (wd.min_size != Size2i()) {
			xsh->flags |= PMinSize;
			xsh->min_width = wd.min_size.x;
			xsh->min_height = wd.min_size.y;
		}
		if (wd.max_size != Size2i()) {
			xsh->flags |= PMaxSize;
			xsh->max_width = wd.max_size.x;
			xsh->max_height = wd.max_size.y;
		}
	}

	XSetWMNormalHints(x11_display, wd.x11_window, xsh);
	XFree(xsh);*/
}

Point2i DisplayServerXCB::window_get_position(WindowID p_window) const {
	_THREAD_SAFE_METHOD_

	ERR_FAIL_COND_V(!windows.has(p_window), Point2i());
	const WindowData &wd = windows[p_window];

	return wd.position;
}

void DisplayServerXCB::window_set_position(const Point2i &p_position, WindowID p_window) {
	/*_THREAD_SAFE_METHOD_

	ERR_FAIL_COND(!windows.has(p_window));
	WindowData &wd = windows[p_window];

	int x = 0;
	int y = 0;
	if (!window_get_flag(WINDOW_FLAG_BORDERLESS, p_window)) {
		//exclude window decorations
		XSync(x11_display, False);
		Atom prop = XInternAtom(x11_display, "_NET_FRAME_EXTENTS", True);
		if (prop != None) {
			Atom type;
			int format;
			unsigned long len;
			unsigned long remaining;
			unsigned char *data = nullptr;
			if (XGetWindowProperty(x11_display, wd.x11_window, prop, 0, 4, False, AnyPropertyType, &type, &format, &len, &remaining, &data) == Success) {
				if (format == 32 && len == 4 && data) {
					long *extents = (long *)data;
					x = extents[0];
					y = extents[2];
				}
				XFree(data);
			}
		}
	}
	XMoveWindow(x11_display, wd.x11_window, p_position.x - x, p_position.y - y);
	_update_real_mouse_position(wd);*/
}

void DisplayServerXCB::window_set_max_size(const Size2i p_size, WindowID p_window) {
	/*_THREAD_SAFE_METHOD_

	ERR_FAIL_COND(!windows.has(p_window));
	WindowData &wd = windows[p_window];

	if ((p_size != Size2i()) && ((p_size.x < wd.min_size.x) || (p_size.y < wd.min_size.y))) {
		ERR_PRINT("Maximum window size can't be smaller than minimum window size!");
		return;
	}
	wd.max_size = p_size;

	_update_size_hints(p_window);
	XFlush(x11_display);*/
}

Size2i DisplayServerXCB::window_get_max_size(WindowID p_window) const {
	_THREAD_SAFE_METHOD_

	ERR_FAIL_COND_V(!windows.has(p_window), Size2i());
	const WindowData &wd = windows[p_window];

	return wd.max_size;
}

void DisplayServerXCB::window_set_min_size(const Size2i p_size, WindowID p_window) {
	/*_THREAD_SAFE_METHOD_

	ERR_FAIL_COND(!windows.has(p_window));
	WindowData &wd = windows[p_window];

	if ((p_size != Size2i()) && (wd.max_size != Size2i()) && ((p_size.x > wd.max_size.x) || (p_size.y > wd.max_size.y))) {
		ERR_PRINT("Minimum window size can't be larger than maximum window size!");
		return;
	}
	wd.min_size = p_size;

	_update_size_hints(p_window);
	XFlush(x11_display);*/
}

Size2i DisplayServerXCB::window_get_min_size(WindowID p_window) const {
	_THREAD_SAFE_METHOD_

	ERR_FAIL_COND_V(!windows.has(p_window), Size2i());
	const WindowData &wd = windows[p_window];

	return wd.min_size;
}

void DisplayServerXCB::window_set_size(const Size2i p_size, WindowID p_window) {
	/*_THREAD_SAFE_METHOD_

	ERR_FAIL_COND(!windows.has(p_window));

	Size2i size = p_size;
	size.x = MAX(1, size.x);
	size.y = MAX(1, size.y);

	WindowData &wd = windows[p_window];

	if (wd.size.width == size.width && wd.size.height == size.height) {
		return;
	}

	XWindowAttributes xwa;
	XSync(x11_display, False);
	XGetWindowAttributes(x11_display, wd.x11_window, &xwa);
	int old_w = xwa.width;
	int old_h = xwa.height;

	// Update our videomode width and height
	wd.size = size;

	// Update the size hints first to make sure the window size can be set
	_update_size_hints(p_window);

	// Resize the window
	XResizeWindow(x11_display, wd.x11_window, size.x, size.y);

	for (int timeout = 0; timeout < 50; ++timeout) {
		XSync(x11_display, False);
		XGetWindowAttributes(x11_display, wd.x11_window, &xwa);

		if (old_w != xwa.width || old_h != xwa.height) {
			break;
		}

		usleep(10000);
	}*/
}

Size2i DisplayServerXCB::window_get_size(WindowID p_window) const {
	_THREAD_SAFE_METHOD_

	ERR_FAIL_COND_V(!windows.has(p_window), Size2i());
	const WindowData &wd = windows[p_window];
	return wd.size;
}

Size2i DisplayServerXCB::window_get_real_size(WindowID p_window) const {
	/*_THREAD_SAFE_METHOD_

	ERR_FAIL_COND_V(!windows.has(p_window), Size2i());
	const WindowData &wd = windows[p_window];

	XWindowAttributes xwa;
	XSync(x11_display, False);
	XGetWindowAttributes(x11_display, wd.x11_window, &xwa);
	int w = xwa.width;
	int h = xwa.height;
	Atom prop = XInternAtom(x11_display, "_NET_FRAME_EXTENTS", True);
	if (prop != None) {
		Atom type;
		int format;
		unsigned long len;
		unsigned long remaining;
		unsigned char *data = nullptr;
		if (XGetWindowProperty(x11_display, wd.x11_window, prop, 0, 4, False, AnyPropertyType, &type, &format, &len, &remaining, &data) == Success) {
			if (format == 32 && len == 4 && data) {
				long *extents = (long *)data;
				w += extents[0] + extents[1]; // left, right
				h += extents[2] + extents[3]; // top, bottom
			}
			XFree(data);
		}
	}
	return Size2i(w, h);*/
	return Size2i();
}

// Just a helper to reduce code duplication in `window_is_maximize_allowed`
// and `_set_wm_maximized`.
bool DisplayServerXCB::_window_maximize_check(WindowID p_window, const char *p_atom_name) const {
	/*ERR_FAIL_COND_V(!windows.has(p_window), false);
	const WindowData &wd = windows[p_window];

	Atom property = XInternAtom(x11_display, p_atom_name, False);
	Atom type;
	int format;
	unsigned long len;
	unsigned long remaining;
	unsigned char *data = nullptr;
	bool retval = false;

	if (property == None) {
		return false;
	}

	int result = XGetWindowProperty(
			x11_display,
			wd.x11_window,
			property,
			0,
			1024,
			False,
			XA_ATOM,
			&type,
			&format,
			&len,
			&remaining,
			&data);

	if (result == Success && data) {
		Atom *atoms = (Atom *)data;
		Atom wm_act_max_horz = XInternAtom(x11_display, "_NET_WM_ACTION_MAXIMIZE_HORZ", False);
		Atom wm_act_max_vert = XInternAtom(x11_display, "_NET_WM_ACTION_MAXIMIZE_VERT", False);
		bool found_wm_act_max_horz = false;
		bool found_wm_act_max_vert = false;

		for (uint64_t i = 0; i < len; i++) {
			if (atoms[i] == wm_act_max_horz) {
				found_wm_act_max_horz = true;
			}
			if (atoms[i] == wm_act_max_vert) {
				found_wm_act_max_vert = true;
			}

			if (found_wm_act_max_horz || found_wm_act_max_vert) {
				retval = true;
				break;
			}
		}

		XFree(data);
	}
	
	return retval;*/
	return false;
}

bool DisplayServerXCB::window_is_maximize_allowed(WindowID p_window) const {
	_THREAD_SAFE_METHOD_
	return _window_maximize_check(p_window, "_NET_WM_ALLOWED_ACTIONS");
}

void DisplayServerXCB::_set_wm_maximized(WindowID p_window, bool p_enabled) {
	/*ERR_FAIL_COND(!windows.has(p_window));
	WindowData &wd = windows[p_window];

	// Using EWMH -- Extended Window Manager Hints
	XEvent xev;
	Atom wm_state = XInternAtom(x11_display, "_NET_WM_STATE", False);
	Atom wm_max_horz = XInternAtom(x11_display, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
	Atom wm_max_vert = XInternAtom(x11_display, "_NET_WM_STATE_MAXIMIZED_VERT", False);

	memset(&xev, 0, sizeof(xev));
	xev.type = ClientMessage;
	xev.xclient.window = wd.x11_window;
	xev.xclient.message_type = wm_state;
	xev.xclient.format = 32;
	xev.xclient.data.l[0] = p_enabled ? _NET_WM_STATE_ADD : _NET_WM_STATE_REMOVE;
	xev.xclient.data.l[1] = wm_max_horz;
	xev.xclient.data.l[2] = wm_max_vert;

	XSendEvent(x11_display, DefaultRootWindow(x11_display), False, SubstructureRedirectMask | SubstructureNotifyMask, &xev);

	if (p_enabled && window_is_maximize_allowed(p_window)) {
		// Wait for effective resizing (so the GLX context is too).
		// Give up after 0.5s, it's not going to happen on this WM.
		// https://github.com/godotengine/godot/issues/19978
		for (int attempt = 0; window_get_mode(p_window) != WINDOW_MODE_MAXIMIZED && attempt < 50; attempt++) {
			usleep(10000);
		}
	}*/
}

void DisplayServerXCB::_set_wm_fullscreen(WindowID p_window, bool p_enabled) {
	/*ERR_FAIL_COND(!windows.has(p_window));
	WindowData &wd = windows[p_window];

	if (p_enabled && !window_get_flag(WINDOW_FLAG_BORDERLESS, p_window)) {
		// remove decorations if the window is not already borderless
		Hints hints;
		Atom property;
		hints.flags = 2;
		hints.decorations = 0;
		property = XInternAtom(x11_display, "_MOTIF_WM_HINTS", True);
		if (property != None) {
			XChangeProperty(x11_display, wd.x11_window, property, property, 32, PropModeReplace, (unsigned char *)&hints, 5);
		}
	}

	if (p_enabled) {
		// Set the window as resizable to prevent window managers to ignore the fullscreen state flag.
		_update_size_hints(p_window);
	}

	// Using EWMH -- Extended Window Manager Hints
	XEvent xev;
	Atom wm_state = XInternAtom(x11_display, "_NET_WM_STATE", False);
	Atom wm_fullscreen = XInternAtom(x11_display, "_NET_WM_STATE_FULLSCREEN", False);

	memset(&xev, 0, sizeof(xev));
	xev.type = ClientMessage;
	xev.xclient.window = wd.x11_window;
	xev.xclient.message_type = wm_state;
	xev.xclient.format = 32;
	xev.xclient.data.l[0] = p_enabled ? _NET_WM_STATE_ADD : _NET_WM_STATE_REMOVE;
	xev.xclient.data.l[1] = wm_fullscreen;
	xev.xclient.data.l[2] = 0;

	XSendEvent(x11_display, DefaultRootWindow(x11_display), False, SubstructureRedirectMask | SubstructureNotifyMask, &xev);

	// set bypass compositor hint
	Atom bypass_compositor = XInternAtom(x11_display, "_NET_WM_BYPASS_COMPOSITOR", False);
	unsigned long compositing_disable_on = p_enabled ? 1 : 0;
	if (bypass_compositor != None) {
		XChangeProperty(x11_display, wd.x11_window, bypass_compositor, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&compositing_disable_on, 1);
	}

	XFlush(x11_display);

	if (!p_enabled) {
		// Reset the non-resizable flags if we un-set these before.
		_update_size_hints(p_window);

		// put back or remove decorations according to the last set borderless state
		Hints hints;
		Atom property;
		hints.flags = 2;
		hints.decorations = window_get_flag(WINDOW_FLAG_BORDERLESS, p_window) ? 0 : 1;
		property = XInternAtom(x11_display, "_MOTIF_WM_HINTS", True);
		if (property != None) {
			XChangeProperty(x11_display, wd.x11_window, property, property, 32, PropModeReplace, (unsigned char *)&hints, 5);
		}
	}*/
}

void DisplayServerXCB::window_set_mode(WindowMode p_mode, WindowID p_window) {
	/*_THREAD_SAFE_METHOD_

	ERR_FAIL_COND(!windows.has(p_window));
	WindowData &wd = windows[p_window];

	WindowMode old_mode = window_get_mode(p_window);
	if (old_mode == p_mode) {
		return; // do nothing
	}
	//remove all "extra" modes

	switch (old_mode) {
		case WINDOW_MODE_WINDOWED: {
			//do nothing
		} break;
		case WINDOW_MODE_MINIMIZED: {
			//Un-Minimize
			// Using ICCCM -- Inter-Client Communication Conventions Manual
			XEvent xev;
			Atom wm_change = XInternAtom(x11_display, "WM_CHANGE_STATE", False);

			memset(&xev, 0, sizeof(xev));
			xev.type = ClientMessage;
			xev.xclient.window = wd.x11_window;
			xev.xclient.message_type = wm_change;
			xev.xclient.format = 32;
			xev.xclient.data.l[0] = WM_NormalState;

			XSendEvent(x11_display, DefaultRootWindow(x11_display), False, SubstructureRedirectMask | SubstructureNotifyMask, &xev);

			Atom wm_state = XInternAtom(x11_display, "_NET_WM_STATE", False);
			Atom wm_hidden = XInternAtom(x11_display, "_NET_WM_STATE_HIDDEN", False);

			memset(&xev, 0, sizeof(xev));
			xev.type = ClientMessage;
			xev.xclient.window = wd.x11_window;
			xev.xclient.message_type = wm_state;
			xev.xclient.format = 32;
			xev.xclient.data.l[0] = _NET_WM_STATE_ADD;
			xev.xclient.data.l[1] = wm_hidden;

			XSendEvent(x11_display, DefaultRootWindow(x11_display), False, SubstructureRedirectMask | SubstructureNotifyMask, &xev);
		} break;
		case WINDOW_MODE_FULLSCREEN: {
			//Remove full-screen
			wd.fullscreen = false;

			_set_wm_fullscreen(p_window, false);

			//un-maximize required for always on top
			bool on_top = window_get_flag(WINDOW_FLAG_ALWAYS_ON_TOP, p_window);

			window_set_position(wd.last_position_before_fs, p_window);

			if (on_top) {
				_set_wm_maximized(p_window, false);
			}

		} break;
		case WINDOW_MODE_MAXIMIZED: {
			_set_wm_maximized(p_window, false);
		} break;
	}

	switch (p_mode) {
		case WINDOW_MODE_WINDOWED: {
			//do nothing
		} break;
		case WINDOW_MODE_MINIMIZED: {
			// Using ICCCM -- Inter-Client Communication Conventions Manual
			XEvent xev;
			Atom wm_change = XInternAtom(x11_display, "WM_CHANGE_STATE", False);

			memset(&xev, 0, sizeof(xev));
			xev.type = ClientMessage;
			xev.xclient.window = wd.x11_window;
			xev.xclient.message_type = wm_change;
			xev.xclient.format = 32;
			xev.xclient.data.l[0] = WM_IconicState;

			XSendEvent(x11_display, DefaultRootWindow(x11_display), False, SubstructureRedirectMask | SubstructureNotifyMask, &xev);

			Atom wm_state = XInternAtom(x11_display, "_NET_WM_STATE", False);
			Atom wm_hidden = XInternAtom(x11_display, "_NET_WM_STATE_HIDDEN", False);

			memset(&xev, 0, sizeof(xev));
			xev.type = ClientMessage;
			xev.xclient.window = wd.x11_window;
			xev.xclient.message_type = wm_state;
			xev.xclient.format = 32;
			xev.xclient.data.l[0] = _NET_WM_STATE_ADD;
			xev.xclient.data.l[1] = wm_hidden;

			XSendEvent(x11_display, DefaultRootWindow(x11_display), False, SubstructureRedirectMask | SubstructureNotifyMask, &xev);
		} break;
		case WINDOW_MODE_FULLSCREEN: {
			wd.last_position_before_fs = wd.position;

			if (window_get_flag(WINDOW_FLAG_ALWAYS_ON_TOP, p_window)) {
				_set_wm_maximized(p_window, true);
			}

			wd.fullscreen = true;
			_set_wm_fullscreen(p_window, true);
		} break;
		case WINDOW_MODE_MAXIMIZED: {
			_set_wm_maximized(p_window, true);
		} break;
	}*/
}

DisplayServer::WindowMode DisplayServerXCB::window_get_mode(WindowID p_window) const {
	/*_THREAD_SAFE_METHOD_

	ERR_FAIL_COND_V(!windows.has(p_window), WINDOW_MODE_WINDOWED);
	const WindowData &wd = windows[p_window];

	if (wd.fullscreen) { //if fullscreen, it's not in another mode
		return WINDOW_MODE_FULLSCREEN;
	}

	// Test maximized.
	// Using EWMH -- Extended Window Manager Hints
	if (_window_maximize_check(p_window, "_NET_WM_STATE")) {
		return WINDOW_MODE_MAXIMIZED;
	}

	{ // Test minimized.
		// Using ICCCM -- Inter-Client Communication Conventions Manual
		Atom property = XInternAtom(x11_display, "WM_STATE", True);
		if (property == None) {
			return WINDOW_MODE_WINDOWED;
		}

		Atom type;
		int format;
		unsigned long len;
		unsigned long remaining;
		unsigned char *data = nullptr;

		int result = XGetWindowProperty(
				x11_display,
				wd.x11_window,
				property,
				0,
				32,
				False,
				AnyPropertyType,
				&type,
				&format,
				&len,
				&remaining,
				&data);

		if (result == Success && data) {
			long *state = (long *)data;
			if (state[0] == WM_IconicState) {
				XFree(data);
				return WINDOW_MODE_MINIMIZED;
			}
			XFree(data);
		}
	}
	*/
	// All other discarded, return windowed.

	return WINDOW_MODE_WINDOWED;
}

void DisplayServerXCB::window_set_flag(WindowFlags p_flag, bool p_enabled, WindowID p_window) {
	print_line("DisplayServerXCB::window_set_flag");
	/*_THREAD_SAFE_METHOD_

	ERR_FAIL_COND(!windows.has(p_window));
	WindowData &wd = windows[p_window];

	switch (p_flag) {
		case WINDOW_FLAG_RESIZE_DISABLED: {
			wd.resize_disabled = p_enabled;

			_update_size_hints(p_window);

			XFlush(x11_display);
		} break;
		case WINDOW_FLAG_BORDERLESS: {
			Hints hints;
			Atom property;
			hints.flags = 2;
			hints.decorations = p_enabled ? 0 : 1;
			property = XInternAtom(x11_display, "_MOTIF_WM_HINTS", True);
			if (property != None) {
				XChangeProperty(x11_display, wd.x11_window, property, property, 32, PropModeReplace, (unsigned char *)&hints, 5);
			}

			// Preserve window size
			window_set_size(window_get_size(p_window), p_window);

			wd.borderless = p_enabled;
		} break;
		case WINDOW_FLAG_ALWAYS_ON_TOP: {
			ERR_FAIL_COND_MSG(wd.transient_parent != INVALID_WINDOW_ID, "Can't make a window transient if the 'on top' flag is active.");
			if (p_enabled && wd.fullscreen) {
				_set_wm_maximized(p_window, true);
			}

			Atom wm_state = XInternAtom(x11_display, "_NET_WM_STATE", False);
			Atom wm_above = XInternAtom(x11_display, "_NET_WM_STATE_ABOVE", False);

			XClientMessageEvent xev;
			memset(&xev, 0, sizeof(xev));
			xev.type = ClientMessage;
			xev.window = wd.x11_window;
			xev.message_type = wm_state;
			xev.format = 32;
			xev.data.l[0] = p_enabled ? _NET_WM_STATE_ADD : _NET_WM_STATE_REMOVE;
			xev.data.l[1] = wm_above;
			xev.data.l[3] = 1;
			XSendEvent(x11_display, DefaultRootWindow(x11_display), False, SubstructureRedirectMask | SubstructureNotifyMask, (XEvent *)&xev);

			if (!p_enabled && !wd.fullscreen) {
				_set_wm_maximized(p_window, false);
			}
			wd.on_top = p_enabled;

		} break;
		case WINDOW_FLAG_TRANSPARENT: {
			//todo reimplement
		} break;
		default: {
		}
	}*/
}

bool DisplayServerXCB::window_get_flag(WindowFlags p_flag, WindowID p_window) const {
	/*_THREAD_SAFE_METHOD_

	ERR_FAIL_COND_V(!windows.has(p_window), false);
	const WindowData &wd = windows[p_window];

	switch (p_flag) {
		case WINDOW_FLAG_RESIZE_DISABLED: {
			return wd.resize_disabled;
		} break;
		case WINDOW_FLAG_BORDERLESS: {
			bool borderless = wd.borderless;
			Atom prop = XInternAtom(x11_display, "_MOTIF_WM_HINTS", True);
			if (prop != None) {
				Atom type;
				int format;
				unsigned long len;
				unsigned long remaining;
				unsigned char *data = nullptr;
				if (XGetWindowProperty(x11_display, wd.x11_window, prop, 0, sizeof(Hints), False, AnyPropertyType, &type, &format, &len, &remaining, &data) == Success) {
					if (data && (format == 32) && (len >= 5)) {
						borderless = !((Hints *)data)->decorations;
					}
					if (data) {
						XFree(data);
					}
				}
			}
			return borderless;
		} break;
		case WINDOW_FLAG_ALWAYS_ON_TOP: {
			return wd.on_top;
		} break;
		case WINDOW_FLAG_TRANSPARENT: {
			//todo reimplement
		} break;
		default: {
		}
	}
	*/
	return false;
}

void DisplayServerXCB::window_request_attention(WindowID p_window) {
	print_line("DisplayServerXCB::window_request_attention");
	/*_THREAD_SAFE_METHOD_

	ERR_FAIL_COND(!windows.has(p_window));
	WindowData &wd = windows[p_window];
	// Using EWMH -- Extended Window Manager Hints
	//
	// Sets the _NET_WM_STATE_DEMANDS_ATTENTION atom for WM_STATE
	// Will be unset by the window manager after user react on the request for attention

	XEvent xev;
	Atom wm_state = XInternAtom(x11_display, "_NET_WM_STATE", False);
	Atom wm_attention = XInternAtom(x11_display, "_NET_WM_STATE_DEMANDS_ATTENTION", False);

	memset(&xev, 0, sizeof(xev));
	xev.type = ClientMessage;
	xev.xclient.window = wd.x11_window;
	xev.xclient.message_type = wm_state;
	xev.xclient.format = 32;
	xev.xclient.data.l[0] = _NET_WM_STATE_ADD;
	xev.xclient.data.l[1] = wm_attention;

	XSendEvent(x11_display, DefaultRootWindow(x11_display), False, SubstructureRedirectMask | SubstructureNotifyMask, &xev);
	XFlush(x11_display);*/
}

void DisplayServerXCB::window_move_to_foreground(WindowID p_window) {
	print_line("DisplayServerXCB::window_move_to_foreground");
	/*_THREAD_SAFE_METHOD_

	ERR_FAIL_COND(!windows.has(p_window));
	WindowData &wd = windows[p_window];

	XEvent xev;
	Atom net_active_window = XInternAtom(x11_display, "_NET_ACTIVE_WINDOW", False);

	memset(&xev, 0, sizeof(xev));
	xev.type = ClientMessage;
	xev.xclient.window = wd.x11_window;
	xev.xclient.message_type = net_active_window;
	xev.xclient.format = 32;
	xev.xclient.data.l[0] = 1;
	xev.xclient.data.l[1] = CurrentTime;

	XSendEvent(x11_display, DefaultRootWindow(x11_display), False, SubstructureRedirectMask | SubstructureNotifyMask, &xev);
	XFlush(x11_display);*/
}

bool DisplayServerXCB::window_can_draw(WindowID p_window) const {
	//this seems to be all that is provided by X11
	return window_get_mode(p_window) != WINDOW_MODE_MINIMIZED;
}

bool DisplayServerXCB::can_any_window_draw() const {
	_THREAD_SAFE_METHOD_

	for (Map<WindowID, WindowData>::Element *E = windows.front(); E; E = E->next()) {
		if (window_get_mode(E->key()) != WINDOW_MODE_MINIMIZED) {
			return true;
		}
	}

	return false;
}

void DisplayServerXCB::window_set_ime_active(const bool p_active, WindowID p_window) {
	/*_THREAD_SAFE_METHOD_

	ERR_FAIL_COND(!windows.has(p_window));
	WindowData &wd = windows[p_window];

	wd.im_active = p_active;

	if (!wd.xic) {
		return;
	}

	// Block events polling while changing input focus
	// because it triggers some event polling internally.
	if (p_active) {
		{
			MutexLock mutex_lock(events_mutex);
			XSetICFocus(wd.xic);
		}
		window_set_ime_position(wd.im_position, p_window);
	} else {
		MutexLock mutex_lock(events_mutex);
		XUnsetICFocus(wd.xic);
	}*/
}

void DisplayServerXCB::window_set_ime_position(const Point2i &p_pos, WindowID p_window) {
	/*_THREAD_SAFE_METHOD_

	ERR_FAIL_COND(!windows.has(p_window));
	WindowData &wd = windows[p_window];

	wd.im_position = p_pos;

	if (!wd.xic) {
		return;
	}

	::XPoint spot;
	spot.x = short(p_pos.x);
	spot.y = short(p_pos.y);
	XVaNestedList preedit_attr = XVaCreateNestedList(0, XNSpotLocation, &spot, nullptr);

	{
		// Block events polling during this call
		// because it triggers some event polling internally.
		MutexLock mutex_lock(events_mutex);
		XSetICValues(wd.xic, XNPreeditAttributes, preedit_attr, nullptr);
	}

	XFree(preedit_attr);*/
}

void DisplayServerXCB::cursor_set_shape(CursorShape p_shape) {
	/*_THREAD_SAFE_METHOD_

	ERR_FAIL_INDEX(p_shape, CURSOR_MAX);

	if (p_shape == current_cursor) {
		return;
	}

	if (mouse_mode == MOUSE_MODE_VISIBLE || mouse_mode == MOUSE_MODE_CONFINED) {
		if (cursors[p_shape] != None) {
			for (Map<WindowID, WindowData>::Element *E = windows.front(); E; E = E->next()) {
				XDefineCursor(x11_display, E->get().x11_window, cursors[p_shape]);
			}
		} else if (cursors[CURSOR_ARROW] != None) {
			for (Map<WindowID, WindowData>::Element *E = windows.front(); E; E = E->next()) {
				XDefineCursor(x11_display, E->get().x11_window, cursors[CURSOR_ARROW]);
			}
		}
	}

	current_cursor = p_shape;*/
}

DisplayServerXCB::CursorShape DisplayServerXCB::cursor_get_shape() const {
	return current_cursor;
}

void DisplayServerXCB::cursor_set_custom_image(const RES &p_cursor, CursorShape p_shape, const Vector2 &p_hotspot) {
	/*_THREAD_SAFE_METHOD_

	if (p_cursor.is_valid()) {
		Map<CursorShape, Vector<Variant>>::Element *cursor_c = cursors_cache.find(p_shape);

		if (cursor_c) {
			if (cursor_c->get()[0] == p_cursor && cursor_c->get()[1] == p_hotspot) {
				cursor_set_shape(p_shape);
				return;
			}

			cursors_cache.erase(p_shape);
		}

		Ref<Texture2D> texture = p_cursor;
		Ref<AtlasTexture> atlas_texture = p_cursor;
		Ref<Image> image;
		Size2i texture_size;
		Rect2i atlas_rect;

		if (texture.is_valid()) {
			image = texture->get_data();
		}

		if (!image.is_valid() && atlas_texture.is_valid()) {
			texture = atlas_texture->get_atlas();

			atlas_rect.size.width = texture->get_width();
			atlas_rect.size.height = texture->get_height();
			atlas_rect.position.x = atlas_texture->get_region().position.x;
			atlas_rect.position.y = atlas_texture->get_region().position.y;

			texture_size.width = atlas_texture->get_region().size.x;
			texture_size.height = atlas_texture->get_region().size.y;
		} else if (image.is_valid()) {
			texture_size.width = texture->get_width();
			texture_size.height = texture->get_height();
		}

		ERR_FAIL_COND(!texture.is_valid());
		ERR_FAIL_COND(p_hotspot.x < 0 || p_hotspot.y < 0);
		ERR_FAIL_COND(texture_size.width > 256 || texture_size.height > 256);
		ERR_FAIL_COND(p_hotspot.x > texture_size.width || p_hotspot.y > texture_size.height);

		image = texture->get_data();

		ERR_FAIL_COND(!image.is_valid());

		// Create the cursor structure
		XcursorImage *cursor_image = XcursorImageCreate(texture_size.width, texture_size.height);
		XcursorUInt image_size = texture_size.width * texture_size.height;
		XcursorDim size = sizeof(XcursorPixel) * image_size;

		cursor_image->version = 1;
		cursor_image->size = size;
		cursor_image->xhot = p_hotspot.x;
		cursor_image->yhot = p_hotspot.y;

		// allocate memory to contain the whole file
		cursor_image->pixels = (XcursorPixel *)memalloc(size);

		for (XcursorPixel index = 0; index < image_size; index++) {
			int row_index = floor(index / texture_size.width) + atlas_rect.position.y;
			int column_index = (index % int(texture_size.width)) + atlas_rect.position.x;

			if (atlas_texture.is_valid()) {
				column_index = MIN(column_index, atlas_rect.size.width - 1);
				row_index = MIN(row_index, atlas_rect.size.height - 1);
			}

			*(cursor_image->pixels + index) = image->get_pixel(column_index, row_index).to_argb32();
		}

		ERR_FAIL_COND(cursor_image->pixels == nullptr);

		// Save it for a further usage
		cursors[p_shape] = XcursorImageLoadCursor(x11_display, cursor_image);

		Vector<Variant> params;
		params.push_back(p_cursor);
		params.push_back(p_hotspot);
		cursors_cache.insert(p_shape, params);

		if (p_shape == current_cursor) {
			if (mouse_mode == MOUSE_MODE_VISIBLE || mouse_mode == MOUSE_MODE_CONFINED) {
				for (Map<WindowID, WindowData>::Element *E = windows.front(); E; E = E->next()) {
					XDefineCursor(x11_display, E->get().x11_window, cursors[p_shape]);
				}
			}
		}

		memfree(cursor_image->pixels);
		XcursorImageDestroy(cursor_image);
	} else {
		// Reset to default system cursor
		if (img[p_shape]) {
			cursors[p_shape] = XcursorImageLoadCursor(x11_display, img[p_shape]);
		}

		CursorShape c = current_cursor;
		current_cursor = CURSOR_MAX;
		cursor_set_shape(c);

		cursors_cache.erase(p_shape);
	}*/
}

int DisplayServerXCB::keyboard_get_layout_count() const {
	/*int _group_count = 0;
	XkbDescRec *kbd = XkbAllocKeyboard();
	if (kbd) {
		kbd->dpy = x11_display;
		XkbGetControls(x11_display, XkbAllControlsMask, kbd);
		XkbGetNames(x11_display, XkbSymbolsNameMask, kbd);

		const Atom *groups = kbd->names->groups;
		if (kbd->ctrls != NULL) {
			_group_count = kbd->ctrls->num_groups;
		} else {
			while (_group_count < XkbNumKbdGroups && groups[_group_count] != None) {
				_group_count++;
			}
		}
		XkbFreeKeyboard(kbd, 0, true);
	}
	return _group_count;*/
	return 0;
}

int DisplayServerXCB::keyboard_get_current_layout() const {
	/*XkbStateRec state;
	XkbGetState(x11_display, XkbUseCoreKbd, &state);
	return state.group;*/
	return 0;
}

void DisplayServerXCB::keyboard_set_current_layout(int p_index) {
	/*ERR_FAIL_INDEX(p_index, keyboard_get_layout_count());
	XkbLockGroup(x11_display, XkbUseCoreKbd, p_index);*/
}

String DisplayServerXCB::keyboard_get_layout_language(int p_index) const {
	/*String ret;
	XkbDescRec *kbd = XkbAllocKeyboard();
	if (kbd) {
		kbd->dpy = x11_display;
		XkbGetControls(x11_display, XkbAllControlsMask, kbd);
		XkbGetNames(x11_display, XkbSymbolsNameMask, kbd);
		XkbGetNames(x11_display, XkbGroupNamesMask, kbd);

		int _group_count = 0;
		const Atom *groups = kbd->names->groups;
		if (kbd->ctrls != NULL) {
			_group_count = kbd->ctrls->num_groups;
		} else {
			while (_group_count < XkbNumKbdGroups && groups[_group_count] != None) {
				_group_count++;
			}
		}

		Atom names = kbd->names->symbols;
		if (names != None) {
			char *name = XGetAtomName(x11_display, names);
			Vector<String> info = String(name).split("+");
			if (p_index >= 0 && p_index < _group_count) {
				if (p_index + 1 < info.size()) {
					ret = info[p_index + 1]; // Skip "pc" at the start and "inet"/"group" at the end of symbols.
				} else {
					ret = "en"; // No symbol for layout fallback to "en".
				}
			} else {
				ERR_PRINT("Index " + itos(p_index) + "is out of bounds (" + itos(_group_count) + ").");
			}
			XFree(name);
		}
		XkbFreeKeyboard(kbd, 0, true);
	}
	return ret.substr(0, 2);*/
	return "";
}

String DisplayServerXCB::keyboard_get_layout_name(int p_index) const {
	/*String ret;
	XkbDescRec *kbd = XkbAllocKeyboard();
	if (kbd) {
		kbd->dpy = x11_display;
		XkbGetControls(x11_display, XkbAllControlsMask, kbd);
		XkbGetNames(x11_display, XkbSymbolsNameMask, kbd);
		XkbGetNames(x11_display, XkbGroupNamesMask, kbd);

		int _group_count = 0;
		const Atom *groups = kbd->names->groups;
		if (kbd->ctrls != NULL) {
			_group_count = kbd->ctrls->num_groups;
		} else {
			while (_group_count < XkbNumKbdGroups && groups[_group_count] != None) {
				_group_count++;
			}
		}

		if (p_index >= 0 && p_index < _group_count) {
			char *full_name = XGetAtomName(x11_display, groups[p_index]);
			ret.parse_utf8(full_name);
			XFree(full_name);
		} else {
			ERR_PRINT("Index " + itos(p_index) + "is out of bounds (" + itos(_group_count) + ").");
		}
		XkbFreeKeyboard(kbd, 0, true);
	}
	return ret;*/
	return "";
}

void DisplayServerXCB::_get_key_modifier_state(unsigned int p_x11_state, Ref<InputEventWithModifiers> state) {
	state->set_shift((p_x11_state & ShiftMask));
	state->set_control((p_x11_state & ControlMask));
	state->set_alt((p_x11_state & Mod1Mask /*|| p_x11_state&Mod5Mask*/)); //altgr should not count as alt
	state->set_metakey((p_x11_state & Mod4Mask));
}

unsigned int DisplayServerXCB::_get_mouse_button_state(unsigned int p_x11_button, int p_x11_type) {
	unsigned int mask = 1 << (p_x11_button - 1);

	if (p_x11_type == ButtonPress) {
		last_button_state |= mask;
	} else {
		last_button_state &= ~mask;
	}

	return last_button_state;
}

void DisplayServerXCB::_window_changed(xcb_generic_event_t *event) {
	xcb_configure_notify_event_t *configure_notify = (xcb_configure_notify_event_t *)event;
	print_line("DisplayServerXCB::_window_changed");
	/*

	// Assign the event to the relevant window
	for (Map<WindowID, WindowData>::Element *E = windows.front(); E; E = E->next()) {
		if (event->xany.window == E->get().x11_window) {
			window_id = E->key();
			break;
		}
	}*/
	WindowID window_id = MAIN_WINDOW_ID;

	Rect2i new_rect;
	new_rect.position = { configure_notify->x, configure_notify->y };
	new_rect.size = { configure_notify->width, configure_notify->height };

	WindowData &wd = windows[window_id];
	wd.position = new_rect.position;
	wd.size = new_rect.size;

#if defined(VULKAN_ENABLED)
	if (rendering_driver == "vulkan") {
		context_vulkan->window_resize(window_id, wd.size.width, wd.size.height);
	}
#endif

	print_line("DisplayServer::_window_changed: " + itos(window_id) + " rect: " + new_rect);
	if (!wd.rect_changed_callback.is_null()) {
		Rect2i r = new_rect;

		Variant rect = r;

		Variant *rectp = &rect;
		Variant ret;
		Callable::CallError ce;
		wd.rect_changed_callback.call((const Variant **)&rectp, 1, ret, ce);
	}
}

void DisplayServerXCB::_dispatch_input_events(const Ref<InputEvent> &p_event) {
	((DisplayServerXCB *)(get_singleton()))->_dispatch_input_event(p_event);
}

void DisplayServerXCB::_dispatch_input_event(const Ref<InputEvent> &p_event) {
	Variant ev = p_event;
	Variant *evp = &ev;
	Variant ret;
	Callable::CallError ce;

	Ref<InputEventFromWindow> event_from_window = p_event;
	if (event_from_window.is_valid() && event_from_window->get_window_id() != INVALID_WINDOW_ID) {
		//send to a window
		ERR_FAIL_COND(!windows.has(event_from_window->get_window_id()));
		Callable callable = windows[event_from_window->get_window_id()].input_event_callback;
		if (callable.is_null()) {
			return;
		}
		callable.call((const Variant **)&evp, 1, ret, ce);
	} else {
		//send to all windows
		for (Map<WindowID, WindowData>::Element *E = windows.front(); E; E = E->next()) {
			Callable callable = E->get().input_event_callback;
			if (callable.is_null()) {
				continue;
			}
			callable.call((const Variant **)&evp, 1, ret, ce);
		}
	}
}

void DisplayServerXCB::_send_window_event(const WindowData &wd, WindowEvent p_event) {
	if (!wd.event_callback.is_null()) {
		Variant event = int(p_event);
		Variant *eventp = &event;
		Variant ret;
		Callable::CallError ce;
		wd.event_callback.call((const Variant **)&eventp, 1, ret, ce);
	}
}

void DisplayServerXCB::_poll_events_thread(void *ud) {
	DisplayServerXCB *display_server = (DisplayServerXCB *)ud;
	display_server->_poll_events();
}

bool DisplayServerXCB::_wait_for_events() const {
	print_line("DisplayServerXCB::_wait_for_events");
	xcb_flush(xcb_connection);
	/*int x11_fd = ConnectionNumber(x11_display);
	fd_set in_fds;

	XFlush(x11_display);

	FD_ZERO(&in_fds);
	FD_SET(x11_fd, &in_fds);

	struct timeval tv;
	tv.tv_usec = 0;
	tv.tv_sec = 1;

	// Wait for next event or timeout.
	int num_ready_fds = select(x11_fd + 1, &in_fds, NULL, NULL, &tv);

	if (num_ready_fds > 0) {
		// Event received.
		return true;
	} else {
		// Error or timeout.
		if (num_ready_fds < 0) {
			ERR_PRINT("_wait_for_events: select error: " + itos(errno));
		}
		return false;
	}*/
	return false;
}

void DisplayServerXCB::_poll_events() {
	/*while (!events_thread_done.is_set()) {
		_wait_for_events();

		// Process events from the queue.
		{
			MutexLock mutex_lock(events_mutex);

			// Non-blocking wait for next event and remove it from the queue.
			XEvent ev;
			while (XCheckIfEvent(x11_display, &ev, _predicate_all_events, nullptr)) {
				// Check if the input manager wants to process the event.
				if (XFilterEvent(&ev, None)) {
					// Event has been filtered by the Input Manager,
					// it has to be ignored and a new one will be received.
					continue;
				}

				// Handle selection request events directly in the event thread, because
				// communication through the x server takes several events sent back and forth
				// and we don't want to block other programs while processing only one each frame.
				if (ev.type == SelectionRequest) {
					_handle_selection_request_event(&(ev.xselectionrequest));
					continue;
				}

				polled_events.push_back(ev);
			}
		}
	}*/
}

void DisplayServerXCB::process_events() {
	//print_line("DisplayServerXCB::process_events");
	xcb_generic_event_t *event;
	DisplayServer::WindowID window_id = MAIN_WINDOW_ID;
	while (event = xcb_poll_for_event(xcb_connection)) {
		switch (event->response_type) {
			case XCB_EXPOSE: {
				print_line("XCB_EXPOSE");

			} break;
			case XCB_CONFIGURE_NOTIFY: {
				print_line("XCB_CONFIGURE_NOTIFY");
				const WindowData &wd = windows[window_id];
				_window_changed(event);
			} break;

			case XCB_MOTION_NOTIFY: {
				// The X11 API requires filtering one-by-one through the motion
				// notify events, in order to figure out which event is the one
				// generated by warping the mouse pointer.
				xcb_motion_notify_event_t *motion_event = (xcb_motion_notify_event_t *)event;
				int event_x = motion_event->root_x;
				int event_y = motion_event->root_y;
				printf("XCB_MOTION_NOTIFY %d-%d\n", event_x, event_y);
				if (mouse_mode == MOUSE_MODE_CAPTURED && event_x == windows[MAIN_WINDOW_ID].size.width / 2 && event_y == windows[MAIN_WINDOW_ID].size.height / 2) {
					//this is likely the warp event since it was warped here
					center = Vector2(event_x, event_y);
					break;
				}

				last_timestamp = motion_event->time;

				// Motion is also simple.
				// A little hack is in order
				// to be able to send relative motion events.
				Point2i pos(event_x, event_y);

				// Avoidance of spurious mouse motion (see handling of touch)
				bool filter = false;
				// Adding some tolerance to match better Point2i to Vector2
				if (xi.state.size() && Vector2(pos).distance_squared_to(xi.mouse_pos_to_filter) < 2) {
					filter = true;
				}
				// Invalidate to avoid filtering a possible legitimate similar event coming later
				xi.mouse_pos_to_filter = Vector2(1e10, 1e10);
				if (filter) {
					break;
				}

				const WindowData &wd = windows[window_id];
				bool focused = wd.focused;

				if (mouse_mode == MOUSE_MODE_CAPTURED) {
					if (xi.relative_motion.x == 0 && xi.relative_motion.y == 0) {
						break;
					}

					Point2i new_center = pos;
					pos = last_mouse_pos + xi.relative_motion;
					center = new_center;
					do_mouse_warp = focused; // warp the cursor if we're focused in
				}

				if (!last_mouse_pos_valid) {
					last_mouse_pos = pos;
					last_mouse_pos_valid = true;
				}

				// Hackish but relative mouse motion is already handled in the RawMotion event.
				//  RawMotion does not provide the absolute mouse position (whereas MotionNotify does).
				//  Therefore, RawMotion cannot be the authority on absolute mouse position.
				//  RawMotion provides more precision than MotionNotify, which doesn't sense subpixel motion.
				//  Therefore, MotionNotify cannot be the authority on relative mouse motion.
				//  This means we need to take a combined approach...
				Point2i rel;

				// Only use raw input if in capture mode. Otherwise use the classic behavior.
				if (mouse_mode == MOUSE_MODE_CAPTURED) {
					rel = xi.relative_motion;
				} else {
					rel = pos - last_mouse_pos;
				}

				// Reset to prevent lingering motion
				xi.relative_motion.x = 0;
				xi.relative_motion.y = 0;

				if (mouse_mode == MOUSE_MODE_CAPTURED) {
					pos = Point2i(windows[MAIN_WINDOW_ID].size.width / 2, windows[MAIN_WINDOW_ID].size.height / 2);
				}

				Ref<InputEventMouseMotion> mm;
				mm.instance();

				mm->set_window_id(window_id);
				if (xi.pressure_supported) {
					mm->set_pressure(xi.pressure);
				} else {
					mm->set_pressure((mouse_get_button_state() & (1 << (BUTTON_LEFT - 1))) ? 1.0f : 0.0f);
				}
				mm->set_tilt(xi.tilt);

				_get_key_modifier_state(motion_event->state, mm);
				mm->set_button_mask(mouse_get_button_state());
				mm->set_position(pos);
				mm->set_global_position(pos);
				Input::get_singleton()->set_mouse_position(pos);
				mm->set_speed(Input::get_singleton()->get_last_mouse_speed());

				mm->set_relative(rel);

				last_mouse_pos = pos;

				// printf("rel: %d,%d\n", rel.x, rel.y );
				// Don't propagate the motion event unless we have focus
				// this is so that the relative motion doesn't get messed up
				// after we regain focus.
				if (focused) {
					Input::get_singleton()->accumulate_input_event(mm);
				} else {
					// Propagate the event to the focused window,
					// because it's received only on the topmost window.
					// Note: This is needed for drag & drop to work between windows,
					// because the engine expects events to keep being processed
					// on the same window dragging started.
					for (Map<WindowID, WindowData>::Element *E = windows.front(); E; E = E->next()) {
						const WindowData &wd_other = E->get();
						if (wd_other.focused) {
							xcb_translate_coordinates_cookie_t window = xcb_translate_coordinates(xcb_connection, wd.xcb_window, wd_other.xcb_window, event_x, event_y);
							xcb_translate_coordinates_reply_t *reply = xcb_translate_coordinates_reply(xcb_connection, window, nullptr);
							int x = reply->dst_x;
							int y = reply->dst_y;

							Point2i pos_focused(x, y);

							mm->set_window_id(E->key());
							mm->set_position(pos_focused);
							mm->set_global_position(pos_focused);
							mm->set_speed(Input::get_singleton()->get_last_mouse_speed());
							Input::get_singleton()->accumulate_input_event(mm);

							break;
						}
					}
				}
			} break;
		}
	}
	xcb_flush(xcb_connection);
	Input::get_singleton()->flush_accumulated_events();
}

void DisplayServerXCB::release_rendering_thread() {
}

void DisplayServerXCB::make_rendering_thread() {
}

void DisplayServerXCB::swap_buffers() {
}

void DisplayServerXCB::_update_context(WindowData &wd) {
}

void DisplayServerXCB::set_context(Context p_context) {
	_THREAD_SAFE_METHOD_

	context = p_context;

	for (Map<WindowID, WindowData>::Element *E = windows.front(); E; E = E->next()) {
		_update_context(E->get());
	}
}

void DisplayServerXCB::set_native_icon(const String &p_filename) {
	WARN_PRINT("Native icon not supported by this display server.");
}

bool g_set_icon_error = false;
int set_icon_errorhandler(Display *dpy, XErrorEvent *ev) {
	g_set_icon_error = true;
	return 0;
}

void DisplayServerXCB::set_icon(const Ref<Image> &p_icon) {
}

Vector<String> DisplayServerXCB::get_rendering_drivers_func() {
	Vector<String> drivers;

#ifdef VULKAN_ENABLED
	drivers.push_back("vulkan");
#endif
#ifdef OPENGL_ENABLED
	drivers.push_back("opengl");
#endif

	return drivers;
}

DisplayServer *DisplayServerXCB::create_func(const String &p_rendering_driver, WindowMode p_mode, uint32_t p_flags, const Vector2i &p_resolution, Error &r_error) {
	DisplayServer *ds = memnew(DisplayServerXCB(p_rendering_driver, p_mode, p_flags, p_resolution, r_error));
	if (r_error != OK) {
		ds->alert("Your video card driver does not support any of the supported Vulkan versions.\n"
				  "Please update your drivers or if you have a very old or integrated GPU upgrade it.",
				"Unable to initialize Video driver");
	}
	return ds;
}

DisplayServerXCB::WindowID DisplayServerXCB::_create_window(WindowMode p_mode, uint32_t p_flags, const Rect2i &p_rect) {
	print_line("DisplayServerXCB::create_window");
	WindowID id = window_id_counter++;
	WindowData &wd = windows[id];

	uint32_t mask;

	xcb_colormap_t colormap_id = xcb_generate_id(xcb_connection);
	xcb_create_colormap(xcb_connection, XCB_COLORMAP_ALLOC_NONE, colormap_id, xcb_screen->root, xcb_screen->root_visual);
	mask = XCB_CW_BORDER_PIXEL | XCB_CW_EVENT_MASK | XCB_CW_COLORMAP;

	uint32_t event_mask =
			XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE | XCB_EVENT_MASK_BUTTON_PRESS |
			XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_ENTER_WINDOW |
			XCB_EVENT_MASK_LEAVE_WINDOW | XCB_EVENT_MASK_POINTER_MOTION |
			XCB_EVENT_MASK_BUTTON_1_MOTION |
			XCB_EVENT_MASK_BUTTON_2_MOTION | XCB_EVENT_MASK_BUTTON_3_MOTION |
			XCB_EVENT_MASK_BUTTON_4_MOTION | XCB_EVENT_MASK_BUTTON_5_MOTION |
			XCB_EVENT_MASK_BUTTON_MOTION | XCB_EVENT_MASK_KEYMAP_STATE |
			XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_VISIBILITY_CHANGE |
			XCB_EVENT_MASK_STRUCTURE_NOTIFY |
			XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
			XCB_EVENT_MASK_FOCUS_CHANGE | XCB_EVENT_MASK_PROPERTY_CHANGE |
			XCB_EVENT_MASK_COLOR_MAP_CHANGE | XCB_EVENT_MASK_OWNER_GRAB_BUTTON;
	const uint32_t value_list[] = {
		0,
		event_mask,
		colormap_id
	};

	wd.xcb_window = xcb_generate_id(xcb_connection);
	wd.cookie = xcb_create_window(xcb_connection,
			XCB_COPY_FROM_PARENT, wd.xcb_window, xcb_screen->root,
			p_rect.position.x, p_rect.position.y, p_rect.size.width, p_rect.size.height,
			0,
			XCB_WINDOW_CLASS_INPUT_OUTPUT,
			xcb_screen->root_visual,
			mask, value_list);

	print_line("New window xcb_window_t = " + String::num_uint64(wd.xcb_window));

#if defined(VULKAN_ENABLED)
	if (context_vulkan) {
		Error err = context_vulkan->window_create(id, wd.xcb_window, xcb_connection, p_rect.size.width, p_rect.size.height);
		ERR_FAIL_COND_V_MSG(err != OK, INVALID_WINDOW_ID, "Can't create a Vulkan window");
	}
#endif
	xcb_flush(xcb_connection);
	return id;
}

DisplayServerXCB::DisplayServerXCB(const String &p_rendering_driver, WindowMode p_mode, uint32_t p_flags, const Vector2i &p_resolution, Error &r_error) {
	print_line("DisplayServerXCB::DisplayServerXCB");
	Input::get_singleton()->set_event_dispatch_function(_dispatch_input_events);

	r_error = OK;

	current_cursor = CURSOR_ARROW;
	mouse_mode = MOUSE_MODE_VISIBLE;

	for (int i = 0; i < CURSOR_MAX; i++) {
		cursors[i] = None;
		//img[i] = nullptr;
	}

	last_button_state = 0;

	xmbstring = nullptr;

	last_click_ms = 0;
	last_click_button_index = -1;
	last_click_pos = Point2i(-100, -100);

	last_timestamp = 0;
	last_mouse_pos_valid = false;
	xcb_connection = xcb_connect(nullptr, nullptr);
	if (!xcb_connection) {
		ERR_PRINT("XCB Display is not available");
		r_error = ERR_UNAVAILABLE;
		return;
	}

	xcb_screen = xcb_setup_roots_iterator(xcb_get_setup(xcb_connection)).data;

#ifndef _MSC_VER
#warning Forcing vulkan rendering driver because OpenGL not implemented yet
#endif
	rendering_driver = "vulkan";

#if defined(VULKAN_ENABLED)
	if (rendering_driver == "vulkan") {
		context_vulkan = memnew(VulkanContextXCB);
		if (context_vulkan->initialize() != OK) {
			memdelete(context_vulkan);
			context_vulkan = nullptr;
			r_error = ERR_CANT_CREATE;
			ERR_FAIL_MSG("Could not initialize Vulkan");
		}
	}
#endif

	Point2i window_position(
			(screen_get_size(0).width - p_resolution.width) / 2,
			(screen_get_size(0).height - p_resolution.height) / 2);
	WindowID main_window = _create_window(p_mode, p_flags, Rect2i(window_position, p_resolution));
	if (main_window == INVALID_WINDOW_ID) {
		r_error = ERR_CANT_CREATE;
		return;
	}
	for (int i = 0; i < WINDOW_FLAG_MAX; i++) {
		if (p_flags & (1 << i)) {
			window_set_flag(WindowFlags(i), true, main_window);
		}
	}
	show_window(main_window);

//create RenderingDevice if used
#if defined(VULKAN_ENABLED)
	if (rendering_driver == "vulkan") {
		//temporary
		rendering_device_vulkan = memnew(RenderingDeviceVulkan);
		rendering_device_vulkan->initialize(context_vulkan);

		RendererCompositorRD::make_current();
	}
#endif

	/* Atorm internment */
	events_thread.start(_poll_events_thread, this);

	_update_real_mouse_position(windows[MAIN_WINDOW_ID]);

	r_error = OK;
}

DisplayServerXCB::~DisplayServerXCB() {
	// Send owned clipboard data to clipboard manager before exit.
	/*Window xcb_main_window = windows[MAIN_WINDOW_ID].xcb_window;
	_clipboard_transfer_ownership(XA_PRIMARY, x11_main_window);
	_clipboard_transfer_ownership(XInternAtom(x11_display, "CLIPBOARD", 0), x11_main_window);*/

	events_thread_done.set();
	events_thread.wait_to_finish();

	//destroy all windows
	for (Map<WindowID, WindowData>::Element *E = windows.front(); E; E = E->next()) {
#ifdef VULKAN_ENABLED
		if (rendering_driver == "vulkan") {
			context_vulkan->window_destroy(E->key());
		}
#endif

		WindowData &wd = E->get();
		/*if (wd.xic) {
			XDestroyIC(wd.xic);
			wd.xic = nullptr;
		}
		XUnmapWindow(x11_display, wd.x11_window);
		XDestroyWindow(x11_display, wd.x11_window);*/
	}

	//destroy drivers
#if defined(VULKAN_ENABLED)
	if (rendering_driver == "vulkan") {
		if (rendering_device_vulkan) {
			rendering_device_vulkan->finalize();
			memdelete(rendering_device_vulkan);
		}

		if (context_vulkan) {
			memdelete(context_vulkan);
		}
	}
#endif

	/*if (xrandr_handle) {
		dlclose(xrandr_handle);
	}
	
	for (int i = 0; i < CURSOR_MAX; i++) {
		if (cursors[i] != None) {
			XFreeCursor(x11_display, cursors[i]);
		}
		if (img[i] != nullptr) {
			XcursorImageDestroy(img[i]);
		}
	};

	if (xim) {
		XCloseIM(xim);
	}

	XCloseDisplay(x11_display);*/
	if (xmbstring) {
		memfree(xmbstring);
	}
}

void DisplayServerXCB::register_xcb_driver() {
	register_create_function("xcb", create_func, get_rendering_drivers_func);
}

#endif // XCB enabled