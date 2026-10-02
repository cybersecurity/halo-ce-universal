// WEB_PRE.JS: runs first in the page and in every thread (tools/web_build.py
// --pre-js). An error on one of the game's threads reaches the page with
// its stack (function names from the name section), for the log.
if (typeof WorkerGlobalScope != 'undefined' && self instanceof WorkerGlobalScope) {
  self.addEventListener('error', (event) => {
    var error = event.error;
    var text = (error && error.stack) ? String(error.stack) : String(event.message);
    postMessage({ cmd: 9, handler: 'haloMessage', args: [4, text] });
  });
}
