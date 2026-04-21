function initJsZipLoadAsyncShim() {
  const globalScope = typeof self !== 'undefined' ? self : window;
  const jszip = globalScope.JSZip;
  if (!jszip) {
    return;
  }

  if (typeof jszip.loadAsync !== 'function') {
    jszip.loadAsync = function (data, options) {
      if (typeof jszip.prototype.loadAsync === 'function') {
        return new jszip().loadAsync(data, options);
      }
      return Promise.reject(new Error('JSZip instance loadAsync is not available.'));
    };
  }
}

initJsZipLoadAsyncShim();
window.initJsZipLoadAsyncShim = initJsZipLoadAsyncShim;
