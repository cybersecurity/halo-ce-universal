/* HTTPS invite fragments stay out of HTTP requests and referrer headers. */
'use strict';
const HaloInvite = (() => {
  function parse(value) {
    const text = String(value || '').trim();
    const native = /^(?:halo:\/\/join\/)?([a-f0-9]{44})$/i.exec(text);
    if (native) return native[1].toLowerCase();
    try {
      const url = new URL(text);
      if (url.protocol !== 'https:' && !(url.protocol === 'http:' &&
          ['localhost', '127.0.0.1', '[::1]'].includes(url.hostname))) return null;
      const token = new URLSearchParams(url.hash.slice(1)).get('join');
      return /^[a-f0-9]{44}$/i.test(token || '') ? token.toLowerCase() : null;
    } catch { return null; }
  }
  function link(base, value) {
    const token = parse(value);
    if (!token) throw new Error('Paste a complete halo://join/ invite (44 hexadecimal characters).');
    const url = new URL(base);
    if (url.protocol !== 'https:' && !(url.protocol === 'http:' &&
        ['localhost', '127.0.0.1', '[::1]'].includes(url.hostname))) throw new Error('A secure website is required.');
    url.search = '';
    url.hash = 'join=' + token;
    return url.toString();
  }
  return { parse, link };
})();
if (typeof module !== 'undefined') module.exports = HaloInvite;
