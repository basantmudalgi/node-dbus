#include <stdlib.h>
#include <v8.h>
#include <node.h>
#include <string>
#include <cstring>
#include <stdlib.h>
#include <dbus/dbus.h>

#include "dbus.h"
#include "decoder.h"
#include "connection.h"
#include "signal.h"

namespace Connection {

	using namespace node;
	using namespace v8;
	using namespace std;

	static void free_later (uv_work_t* req) {
	}

	static void free_watcher_later (uv_work_t* req) {
		if (req->data != NULL) {
			uv_poll_t *watcher = static_cast<uv_poll_t *>(req->data);
			delete watcher;
			req->data = NULL;
		}
		delete req;
	}

	static void free_timer_later (uv_work_t* req) {
		if (req->data != NULL) {
			uv_timer_t *timer = static_cast<uv_timer_t *>(req->data);
			delete timer;
			req->data = NULL;
		}
		delete req;
	}

	static void watcher_handle(uv_poll_t *watcher, int status, int events)
	{
		DBusWatch *watch = static_cast<DBusWatch *>(watcher->data);
		unsigned int flags = 0;

		if (events & UV_READABLE)
			flags |= DBUS_WATCH_READABLE;

		if (events & UV_WRITABLE)
			flags |= DBUS_WATCH_WRITABLE;

		dbus_watch_handle(watch, flags);
	}

	static void watcher_free(void *data)
	{
		uv_poll_t *watcher = static_cast<uv_poll_t *>(data);

		if (watcher == NULL)
			return;

		watcher->data = NULL;

		// Stop watching
		uv_ref((uv_handle_t *)watcher);
		uv_poll_stop(watcher);
		uv_close((uv_handle_t *)watcher, NULL);

		uv_work_t *req = new uv_work_t;
		req->data = (void *)watcher;
		uv_queue_work(uv_default_loop(), req, free_later, (uv_after_work_cb)free_watcher_later);
	}

	static dbus_bool_t watch_add(DBusWatch *watch, void *data)
	{
		if (!dbus_watch_get_enabled(watch) || dbus_watch_get_data(watch) != NULL)
			return true;

		int events = 0;
		int fd = dbus_watch_get_unix_fd(watch);
		unsigned int flags = dbus_watch_get_flags(watch);

		if (flags & DBUS_WATCH_READABLE)
			events |= UV_READABLE;

		if (flags & DBUS_WATCH_WRITABLE)
			events |= UV_WRITABLE;

		// Initializing watcher
		uv_poll_t *watcher = new uv_poll_t;
		watcher->data = (void *)watch;

		// Start watching
		uv_poll_init(uv_default_loop(), watcher, fd);
		uv_poll_start(watcher, events, watcher_handle);
		uv_unref((uv_handle_t *)watcher);

		dbus_watch_set_data(watch, (void *)watcher, watcher_free);

		return true;
	}

	static void watch_remove(DBusWatch *watch, void *data)
	{
		uv_poll_t *watcher = static_cast<uv_poll_t *>(dbus_watch_get_data(watch));

		if (watcher == NULL)
			return;

		dbus_watch_set_data(watch, NULL, NULL);
	}

	static void watch_handle(DBusWatch *watch, void *data)
	{
		if (dbus_watch_get_enabled(watch))
			watch_add(watch, data);
		else
			watch_remove(watch, data);
	}

	static void timer_handle(uv_timer_t *timer, int status)
	{
		DBusTimeout *timeout = static_cast<DBusTimeout *>(timer->data);
		dbus_timeout_handle(timeout);
	}

	static void timer_free(void *data)
	{
		uv_timer_t *timer = static_cast<uv_timer_t *>(data);

		if (timer == NULL)
			return;

		timer->data =  NULL;

		// Stop timer
		uv_timer_stop(timer);
		uv_unref((uv_handle_t *)timer);

		uv_work_t *req = new uv_work_t;
		req->data = (void *)timer;
		uv_queue_work(uv_default_loop(), req, free_later, (uv_after_work_cb)free_timer_later);
	}

	static dbus_bool_t timeout_add(DBusTimeout *timeout, void *data)
	{ 
		if (!dbus_timeout_get_enabled(timeout) || dbus_timeout_get_data(timeout) != NULL)
			return true;

		uv_timer_t *timer = new uv_timer_t;
		timer->data = timeout;

		// Initializing timer
		uv_timer_init(uv_default_loop(), timer);
		uv_timer_start(timer, timer_handle, dbus_timeout_get_interval(timeout), 0);

		dbus_timeout_set_data(timeout, (void *)timer, timer_free);

		return true;
	}

	static void timeout_remove(DBusTimeout *timeout, void *data)
	{
		uv_timer_t *timer = static_cast<uv_timer_t *>(dbus_timeout_get_data(timeout));

		dbus_timeout_set_data(timeout, NULL, NULL);
	}

	static void timeout_toggled(DBusTimeout *timeout, void *data)
	{
		if (dbus_timeout_get_enabled(timeout))
			timeout_add(timeout, data);
		else
			timeout_remove(timeout, data);
	}

	static void connection_loop(uv_async_t *connection_loop_handle, int status)
	{
		DBusConnection *connection = static_cast<DBusConnection *>(connection_loop_handle->data);
		dbus_connection_read_write(connection, 0);

		while(dbus_connection_dispatch(connection) == DBUS_DISPATCH_DATA_REMAINS);
	}

	static void connection_wakeup(void *data)
	{
		uv_async_t *connection_loop_handle = static_cast<uv_async_t *>(data);
		uv_async_send(connection_loop_handle);
	}

	static DBusHandlerResult signal_filter(DBusConnection *connection, DBusMessage *message, void *user_data)
	{
		// Ignore message if it's not a valid signal
		if (dbus_message_get_type(message) != DBUS_MESSAGE_TYPE_SIGNAL) {
			return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
		}

		// Getting the interface name and signal name
		const char *sender = dbus_message_get_sender(message);
		const char *object_path = dbus_message_get_path(message);
		const char *interface = dbus_message_get_interface(message);
		const char *signal_name = dbus_message_get_member(message);
		if (interface == NULL || signal_name == NULL) {
			return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
		}

		// Getting V8 context
		Local<Context> context = Context::GetCurrent();
		Context::Scope ctxScope(context); 
		HandleScope scope;

		// Getting arguments of signal
		Handle<Value> arguments = Decoder::DecodeArguments(message);
		Handle<Value> senderValue = Null();
		if (sender)
			senderValue = String::New(sender);

		Handle<Value> args[] = {
			String::New(dbus_bus_get_unique_name(connection)),
			senderValue,
			String::New(object_path),
			String::New(interface),
			String::New(signal_name),
			arguments
		};

		Signal::DispatchSignal(args);

		return DBUS_HANDLER_RESULT_HANDLED;
	}

	void Init(NodeDBus::BusObject *bus)
	{
		DBusConnection *connection = bus->connection;

		dbus_connection_set_exit_on_disconnect(connection, false);

		// Initializing watcher
		dbus_connection_set_watch_functions(connection, watch_add, watch_remove, watch_handle, NULL, NULL);

		// Initializing timeout handlers
		dbus_connection_set_timeout_functions(connection, timeout_add, timeout_remove, timeout_toggled, NULL, NULL);

		// Initializing loop
		uv_async_t *connection_loop_handle = new uv_async_t;
		connection_loop_handle->data = (void *)connection;
		uv_async_init(uv_default_loop(), connection_loop_handle, connection_loop);
		bus->loop = connection_loop_handle;

		dbus_connection_set_wakeup_main_function(connection, connection_wakeup, connection_loop_handle, free);

		// Initializing signal handler
		dbus_connection_add_filter(connection, signal_filter, NULL, NULL);
	}

	void UnInit(NodeDBus::BusObject *bus)
	{
		DBusConnection *connection = bus->connection;

		uv_unref((uv_handle_t *)bus->loop);

		if (dbus_connection_get_is_connected(connection))
			dbus_connection_close(connection);

		dbus_connection_unref(connection);
	}

}
