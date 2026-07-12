// simsc SPA (M4). Vanilla JS. Uses /simscapp/i18n.js runtime.
//
// Tabs: Games (default) + Keys. All backend calls carry X-API-Key
// from localStorage. On 401 we wipe the key and reload — the /
// welcome page will hand a fresh one over on the next Cognito visit.

(async function () {
  const SUPPORTED_LOCALES = ['en', 'zh-CN'];
  const KEY_STORAGE = 'simsc_api_key';
  const LOCALE_STORAGE = 'simsc_locale';

  // ---- state ----
  const state = {
    key: localStorage.getItem(KEY_STORAGE),
    locale: SUPPORTED_LOCALES.includes(localStorage.getItem(LOCALE_STORAGE))
      ? localStorage.getItem(LOCALE_STORAGE) : 'en',
    dict: null,
    profile: null,
    tab: 'games',
    // last create-game form draft — restored when a decline drops us back
    lastDraft: null,
  };

  // ---- utilities ----
  async function api(path, opts = {}) {
    const resp = await fetch(path, {
      ...opts,
      headers: {
        'X-API-Key': state.key,
        'Content-Type': 'application/json',
        ...(opts.headers || {}),
      },
    });
    if (resp.status === 401) {
      // Bad key — clear + reload; welcome page will auto-populate.
      localStorage.removeItem(KEY_STORAGE);
      location.reload();
      return null;
    }
    if (!resp.ok) {
      let msg = `${resp.status} ${resp.statusText}`;
      try {
        const body = await resp.json();
        if (body.detail) msg = typeof body.detail === 'string' ? body.detail : JSON.stringify(body.detail);
      } catch {}
      throw new Error(msg);
    }
    if (resp.status === 204) return null;
    return await resp.json();
  }

  function showError(msg) {
    const box = document.getElementById('error-box');
    box.textContent = i18n.t(state.dict, 'common.error', { msg });
    box.classList.remove('hidden');
    setTimeout(() => box.classList.add('hidden'), 6000);
  }

  const t = (path, vars) => i18n.t(state.dict, path, vars);

  // ---- boot ----
  state.dict = await i18n.load(state.locale);

  // No-key screen
  if (!state.key) {
    const needKey = document.getElementById('need-key');
    needKey.classList.remove('hidden');
    document.getElementById('need-key-msg').textContent = t('app.no_key_prompt');
    document.getElementById('key-input-label').textContent = t('app.paste_key_label');
    document.getElementById('key-save-btn').textContent = t('app.paste_key_button');
    document.getElementById('key-save-btn').onclick = () => {
      const v = document.getElementById('key-input').value.trim();
      if (!v) return;
      localStorage.setItem(KEY_STORAGE, v);
      location.reload();
    };
    return;
  }

  // Validate key + fetch profile
  try {
    state.profile = await api('/api/me/profile');
  } catch (e) {
    // Any error at this point (network, 500) is fatal; show a bare message
    document.body.innerHTML = `<main><div class="card"><p>${e.message}</p></div></main>`;
    return;
  }

  // Show app shell
  document.getElementById('app').classList.remove('hidden');

  // Populate top-bar
  document.title = t('app.title');
  document.getElementById('app-title').textContent = t('app.title');
  document.getElementById('welcome-line').textContent = t('app.welcome_line',
    { name: state.profile.display_name || state.profile.alias });
  document.getElementById('sign-out-btn').textContent = t('app.sign_out');
  document.getElementById('sign-out-btn').onclick = () => {
    localStorage.removeItem(KEY_STORAGE);
    window.location.href = '/ui/logout';
  };
  document.getElementById('tab-games').textContent = t('app.tab_games');
  document.getElementById('tab-keys').textContent  = t('app.tab_keys');

  // Locale dropdown
  const langSelect = document.getElementById('lang-select');
  for (const code of SUPPORTED_LOCALES) {
    const opt = document.createElement('option');
    opt.value = code;
    const d = code === state.locale ? state.dict : await i18n.load(code);
    opt.textContent = d.meta.language_name;
    if (code === state.locale) opt.selected = true;
    langSelect.appendChild(opt);
  }
  langSelect.onchange = () => {
    localStorage.setItem(LOCALE_STORAGE, langSelect.value);
    location.reload();
  };

  // Tab switching
  document.getElementById('tab-games').onclick = () => switchTab('games');
  document.getElementById('tab-keys').onclick  = () => switchTab('keys');

  function switchTab(tab) {
    state.tab = tab;
    for (const t of ['games', 'keys']) {
      document.getElementById('tab-' + t).classList.toggle('active', t === tab);
      document.getElementById('view-' + t).classList.toggle('hidden', t !== tab);
    }
    if (tab === 'games') renderGames();
    if (tab === 'keys')  renderKeys();
  }

  // ---- games ----
  async function renderGames() {
    const view = document.getElementById('view-games');
    view.innerHTML = `<p class="meta">${t('common.loading')}</p>`;
    let games;
    try { games = await api('/api/games'); }
    catch (e) { showError(e.message); return; }

    // Partition: pending invitations for me (from someone else) vs my games
    const pending = games.filter(g =>
      g.my_invitation_status === 'pending' && g.owner_alias !== state.profile.alias);
    const mine = games.filter(g => !pending.includes(g));

    view.innerHTML = `
      <div style="margin-bottom: 16px">
        <button class="primary" id="create-game-btn">${t('games.create_button')}</button>
      </div>
      <h2>${t('games.invitations_header')}</h2>
      <div id="inv-list">${pending.length ? '' : `<p class="meta">${t('games.no_invitations')}</p>`}</div>
      <h2>${t('games.your_games_header')}</h2>
      <div id="my-list">${mine.length ? '' : `<p class="meta">${t('games.no_games')}</p>`}</div>
    `;
    document.getElementById('create-game-btn').onclick = openCreateModal;

    for (const g of pending) document.getElementById('inv-list').appendChild(gameCard(g, 'invitation'));
    for (const g of mine)    document.getElementById('my-list').appendChild(gameCard(g, 'mine'));
  }

  function gameCard(g, kind) {
    const card = document.createElement('div');
    card.className = 'card';
    const stateBadge = badgeFor(g.state);
    const slots = (g.player_aliases || []).map((a, i) => {
      const r = (g.races || [])[i] || '?';
      const who = a || 'None';
      return `<span class="meta">${i}: ${who} · ${r}</span>`;
    }).join(' &nbsp; ');
    // "Open observer" opens the popup shell at /simscapp/observer.html
    // with just ?game=<id>. The shell reads the API key from
    // localStorage (same origin, same tab-group) and calls
    // /api/games/{id} for details. The key is NEVER in the URL.
    const observerBtn = g.observer_url
      ? `<a class="btn primary" data-observer="${g.game_id}">${t('games.open_observer')}</a>`
      : '';
    let actions = '';
    if (kind === 'invitation') {
      actions = `
        <button class="primary" data-act="accept">${t('games.accept')}</button>
        <button data-act="decline">${t('games.decline')}</button>`;
    } else if (g.state === 'pending_invitations') {
      const pendingAliases = (g.invitations || [])
        .filter(i => i.status === 'pending').map(i => i.alias).join(', ');
      actions = `
        <span class="meta">${t('games.waiting_for', { aliases: pendingAliases })}</span>
        <button class="danger" data-act="cancel">${t('games.cancel')}</button>`;
    } else if (g.state === 'running') {
      actions = `${observerBtn}
        <button class="danger" data-act="delete">${t('games.delete')}</button>`;
    } else {
      actions = `<button class="danger" data-act="delete">${t('games.delete')}</button>`;
    }
    card.innerHTML = `
      <div class="row">
        <div>
          <div><strong>${g.map}</strong> ${stateBadge}</div>
          <div class="meta">${slots}</div>
          <div class="meta mono">${g.game_id}</div>
        </div>
        <div style="display:flex;gap:6px;align-items:center;flex-wrap:wrap">${actions}</div>
      </div>`;

    card.querySelectorAll('[data-observer]').forEach(btn => {
      btn.onclick = (e) => {
        e.preventDefault();
        // window.open must be called synchronously from the click
        // handler or the popup blocker will refuse it.
        const url = '/simscapp/observer.html?game=' +
          encodeURIComponent(btn.dataset.observer);
        window.open(url, 'simsc_observer_' + btn.dataset.observer,
          'width=1400,height=920');
      };
    });
    card.querySelectorAll('[data-act]').forEach(btn => {
      btn.onclick = async () => {
        const act = btn.dataset.act;
        try {
          if (act === 'accept') await api(`/api/games/${g.game_id}/accept`, { method: 'POST' });
          if (act === 'decline') {
            await api(`/api/games/${g.game_id}/decline`, { method: 'POST' });
          }
          if (act === 'cancel') {
            await api(`/api/games/${g.game_id}/cancel`, { method: 'POST' });
            // If we cancelled our own game while it had a draft,
            // pop the modal back so the user can edit.
            if (state.lastDraft && g.owner_alias === state.profile.alias) {
              renderGames();
              openCreateModal(state.lastDraft);
              return;
            }
          }
          if (act === 'delete') await api(`/api/games/${g.game_id}`, { method: 'DELETE' });
          renderGames();
        } catch (e) { showError(e.message); }
      };
    });
    return card;
  }

  function badgeFor(s) {
    const map = {
      pending_invitations: ['pending',    t('games.state_pending')],
      running:             ['running',    t('games.state_running')],
      ended:               ['ended',      t('games.state_ended')],
      cancelled:           ['cancelled',  t('games.state_cancelled')],
    };
    const [cls, txt] = map[s] || ['ended', s];
    return `<span class="badge ${cls}">${txt}</span>`;
  }

  // ---- create-game modal ----
  async function openCreateModal(draft) {
    let maps, users;
    try {
      [maps, users] = await Promise.all([
        api('/api/maps'),
        api('/api/users'),
      ]);
    } catch (e) { showError(e.message); return; }

    const races = ['random', 'protoss', 'terran', 'zerg'];
    const raceLabels = {
      random: t('create_game.race_random'),
      protoss: t('create_game.race_protoss'),
      terran: t('create_game.race_terran'),
      zerg: t('create_game.race_zerg'),
    };

    // Rebuild dropdowns whenever map changes
    const backdrop = document.createElement('div');
    backdrop.className = 'modal-backdrop';
    backdrop.innerHTML = `
      <div class="modal">
        <h2>${t('create_game.title')}</h2>
        <label class="form-label">${t('create_game.map_label')}</label>
        <select id="map-select" style="width:100%">
          ${maps.map(m => `<option value="${m.filename}" data-players="${m.player_count}">${m.filename}</option>`).join('')}
        </select>
        <p class="meta" id="players-hint" style="margin-top:6px"></p>
        <div id="slots"></div>
        <div class="error hidden" id="modal-error"></div>
        <div class="btn-row">
          <button id="modal-cancel">${t('create_game.cancel_button')}</button>
          <button class="primary" id="modal-create">${t('create_game.create_button')}</button>
        </div>
      </div>`;
    document.getElementById('modal-root').appendChild(backdrop);

    const mapSel = backdrop.querySelector('#map-select');
    const slotsEl = backdrop.querySelector('#slots');
    const playersHint = backdrop.querySelector('#players-hint');

    // Draft support: restore last inputs after a decline
    if (draft && draft.map) mapSel.value = draft.map;

    function rebuildSlots() {
      const opt = mapSel.selectedOptions[0];
      const n = parseInt(opt.dataset.players, 10);
      playersHint.textContent = t('create_game.map_players_hint', { n });
      slotsEl.innerHTML = '';
      for (let i = 0; i < n; i++) {
        const row = document.createElement('div');
        row.className = 'slot-row';
        row.innerHTML = `
          <span class="slot-num">${t('create_game.slot_label', { n: i })}</span>
          <select class="slot-player" data-slot="${i}">
            ${playerOptions(i, users)}
          </select>
          <select class="slot-race" data-slot="${i}">
            ${races.map(r => `<option value="${r}"${r==='random'?' selected':''}>${raceLabels[r]}</option>`).join('')}
          </select>`;
        slotsEl.appendChild(row);
      }
      // Restore from draft if provided
      if (draft && draft.player_aliases && draft.player_aliases.length === n) {
        slotsEl.querySelectorAll('.slot-player').forEach((sel, i) => {
          const val = draft.player_aliases[i];
          // For None/AIBot sentinels, or a real alias
          const target = val === null ? '__none' : val === 'AIBot' ? '__aibot' : val;
          if (Array.from(sel.options).some(o => o.value === target)) sel.value = target;
        });
        slotsEl.querySelectorAll('.slot-race').forEach((sel, i) => {
          if (draft.races && draft.races[i]) sel.value = draft.races[i];
        });
      }
    }
    mapSel.onchange = rebuildSlots;
    rebuildSlots();

    function playerOptions(slot, users) {
      // Options: creator (as slot 0 default), other real users, AIBot, None.
      const opts = [];
      const isFirst = slot === 0;
      const me = state.profile.alias;
      opts.push(`<option value="${me}"${isFirst?' selected':''}>${
        t('create_game.slot_you', { alias: me })
      }</option>`);
      for (const u of users) {
        if (u.alias === me) continue;
        opts.push(`<option value="${u.alias}">${u.alias}${
          u.display_name ? ' (' + u.display_name + ')' : ''
        }</option>`);
      }
      opts.push(`<option value="__aibot"${!isFirst?' selected':''}>${t('create_game.slot_aibot')}</option>`);
      opts.push(`<option value="__none">${t('create_game.slot_none')}</option>`);
      return opts.join('');
    }

    backdrop.querySelector('#modal-cancel').onclick = () => backdrop.remove();
    backdrop.querySelector('#modal-create').onclick = async () => {
      const map = mapSel.value;
      const races = Array.from(slotsEl.querySelectorAll('.slot-race')).map(s => s.value);
      const player_aliases = Array.from(slotsEl.querySelectorAll('.slot-player')).map(s => {
        if (s.value === '__none') return null;
        if (s.value === '__aibot') return 'AIBot';
        return s.value;
      });
      const draft = { map, races, player_aliases };
      state.lastDraft = draft;

      const btn = backdrop.querySelector('#modal-create');
      btn.disabled = true; btn.textContent = t('create_game.creating');
      try {
        await api('/api/games', {
          method: 'POST',
          body: JSON.stringify({ map, races, player_aliases }),
        });
        state.lastDraft = null;
        backdrop.remove();
        renderGames();
      } catch (e) {
        const err = backdrop.querySelector('#modal-error');
        err.textContent = t('common.error', { msg: e.message });
        err.classList.remove('hidden');
        btn.disabled = false; btn.textContent = t('create_game.create_button');
      }
    };
  }

  // ---- keys ----
  async function renderKeys() {
    const view = document.getElementById('view-keys');
    view.innerHTML = `<p class="meta">${t('common.loading')}</p>`;
    let keys;
    try { keys = await api('/api/me/keys'); }
    catch (e) { showError(e.message); return; }

    view.innerHTML = `
      <h2>${t('keys.list_header')}</h2>
      <table>
        <thead><tr>
          <th>${t('keys.column_label')}</th>
          <th>${t('keys.column_created')}</th>
          <th>${t('keys.column_status')}</th>
          <th></th>
        </tr></thead>
        <tbody id="keys-body"></tbody>
      </table>
      <div style="margin-top: 16px; display:flex; gap:8px">
        <input type="text" id="new-key-label" placeholder="${t('keys.new_label_placeholder')}"
               style="flex:1">
        <button class="primary" id="new-key-btn">${t('keys.create_button')}</button>
      </div>`;

    const body = view.querySelector('#keys-body');
    for (const k of keys) {
      const row = document.createElement('tr');
      const created = new Date(k.created_at).toISOString().slice(0, 19).replace('T', ' ');
      const active = k.revoked_at === null;
      row.innerHTML = `
        <td>${k.label || '<span class="meta">—</span>'}</td>
        <td class="meta mono">${created}</td>
        <td>${active ? t('keys.status_active') : t('keys.status_revoked')}</td>
        <td>${active ? `<button class="danger" data-id="${k.id}">${t('keys.revoke_button')}</button>` : ''}</td>`;
      const rb = row.querySelector('button');
      if (rb) rb.onclick = async () => {
        try { await api(`/api/me/keys/${rb.dataset.id}`, { method: 'DELETE' }); renderKeys(); }
        catch (e) { showError(e.message); }
      };
      body.appendChild(row);
    }
    view.querySelector('#new-key-btn').onclick = async () => {
      const label = view.querySelector('#new-key-label').value.trim() || null;
      try {
        const created = await api('/api/me/keys', {
          method: 'POST',
          body: JSON.stringify({ label }),
        });
        showRevealBanner(created);
        renderKeys();
      } catch (e) { showError(e.message); }
    };
  }

  function showRevealBanner(created) {
    const banner = document.createElement('div');
    banner.className = 'card';
    banner.style.borderColor = '#2f5d3b';
    banner.innerHTML = `
      <h3 style="margin:0 0 8px 0">${t('keys.just_created_header')}</h3>
      <div class="reveal-box mono" id="reveal-key">${created.plain_key}</div>
      <p class="meta">${t('keys.just_created_hint')}</p>
      <div class="btn-row" style="justify-content:flex-start">
        <button id="copy-btn">${t('keys.copy_button')}</button>
        <button id="dl-btn">${t('keys.download_button')}</button>
        <button class="primary" id="dismiss-btn">${t('keys.dismiss_button')}</button>
      </div>`;
    document.getElementById('view-keys').prepend(banner);
    banner.querySelector('#copy-btn').onclick = async () => {
      try { await navigator.clipboard.writeText(created.plain_key); } catch {}
    };
    banner.querySelector('#dl-btn').onclick = () => {
      const blob = new Blob([created.plain_key + '\n'], { type: 'text/plain' });
      const url = URL.createObjectURL(blob);
      const a = document.createElement('a');
      a.href = url; a.download = `simsc-key-${created.id}.txt`;
      a.click(); URL.revokeObjectURL(url);
    };
    banner.querySelector('#dismiss-btn').onclick = () => banner.remove();
  }

  // Kick off default tab
  switchTab('games');
})();
