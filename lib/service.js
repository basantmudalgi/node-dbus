"use strict";

var util = require('util');
var events = require('events');
var ServiceObject = require('./service_object');

var Service = module.exports = function(bus, serviceName) {
	var self = this;

	self.bus = bus;
	self.serviceName = serviceName;
	self.objects = {};

	self.on('request', function(uniqueName, sender, objectPath, interfaceName, member, message, args) {
		var iface = self.objects[objectPath]['interfaces'][interfaceName];
		if (!iface)
			return;

		iface.call.apply(iface, [ member, message, args ]);
	});
};

util.inherits(Service, events.EventEmitter);

Object.defineProperty(Service.prototype, 'connected', {
	get: function() {
		return this.bus.connected;
	}
});

Service.prototype.getObject = function(objectPath) {
	var self = this;
	return self.objects[objectPath];
}

Service.prototype.createObject = function(objectPath, options) {

	var self = this;
	
	if(options && options.useExisting) {
		if (self.getObject(objectPath)) return self.getObject(objectPath);
		else self.objects[objectPath] = new ServiceObject(self, objectPath);
	} else if(!self.getObject(objectPath)) {
		self.objects[objectPath] = new ServiceObject(self, objectPath);
	}
	
	// Register object
	self.bus._dbus.registerObjectPath(self.bus.connection, objectPath);

	return self.objects[objectPath];
};

Service.prototype.removeObject = function(object) {
	var self = this;

	self.bus._dbus.unregisterObjectPath(self.bus.connection, object.path);

	delete self.objects[object.path];
};

Service.prototype.disconnect = function() {
	var self = this;

	self.bus.disconnect();
};
