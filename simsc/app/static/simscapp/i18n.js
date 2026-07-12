// Tiny i18n runtime shared by welcome page + both SPAs.
// Loads /locales/<code>.json, caches, resolves dotted paths with {var}
// substitution.  Missing key → key itself, so bugs surface loudly.

window.i18n = (function () {
  const cache = new Map();

  async function load(code) {
    if (cache.has(code)) return cache.get(code);
    const resp = await fetch(`/locales/${code}.json`);
    if (!resp.ok) throw new Error(`i18n: failed to load ${code} (${resp.status})`);
    const dict = await resp.json();
    cache.set(code, dict);
    return dict;
  }

  function t(dict, path, vars) {
    const parts = path.split('.');
    let cur = dict;
    for (const p of parts) {
      if (cur == null || typeof cur !== 'object') return path;
      cur = cur[p];
    }
    if (typeof cur !== 'string') return path;
    if (!vars) return cur;
    return cur.replace(/\{(\w+)\}/g, (_, k) => (k in vars ? vars[k] : `{${k}}`));
  }

  return { load, t };
})();
