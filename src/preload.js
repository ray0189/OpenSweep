'use strict';
const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('api', {
  call: (method, args) => ipcRenderer.invoke('api', method, args),
  on: (cb) => ipcRenderer.on('event', (_e, name, data) => cb(name, data)),
});
