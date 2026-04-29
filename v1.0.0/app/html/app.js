(function () {
    'use strict';

    var API = '/local/fifa_wc/api';

    function api(path, opts) {
        return fetch(API + path, Object.assign({ credentials: 'same-origin' }, opts || {}));
    }

    /* ── Tabs ─────────────────────────────────────────────── */
    document.querySelectorAll('.tab').forEach(function (tab) {
        tab.addEventListener('click', function () {
            document.querySelectorAll('.tab').forEach(function (t) { t.classList.remove('active'); });
            document.querySelectorAll('.panel').forEach(function (p) { p.classList.remove('active'); });
            tab.classList.add('active');
            document.getElementById(tab.dataset.tab).classList.add('active');
            if (tab.dataset.tab === 'standings') refreshStandings();
            if (tab.dataset.tab === 'bracket')   refreshBracket();
            if (tab.dataset.tab === 'scorers')   refreshScorers();
        });
    });

    /* ── Utility ──────────────────────────────────────────── */
    function esc(s) {
        return String(s)
            .replace(/&/g,'&amp;').replace(/</g,'&lt;')
            .replace(/>/g,'&gt;').replace(/"/g,'&quot;');
    }

    function statusLabel(st, elapsed, extra) {
        if (st === '1H' || st === '2H' || st === 'ET' || st === 'PEN') {
            var t = elapsed || 0;
            return extra ? t + '+' + extra + "'" : t + "'";
        }
        if (st === 'HT')   return 'HT';
        if (st === 'FT')   return 'FT';
        if (st === 'NS')   return 'Scheduled';
        if (st === 'CANC') return 'Cancelled';
        return st || '--';
    }

    function isLive(st) {
        return st === '1H' || st === '2H' || st === 'ET' || st === 'PEN' || st === 'HT';
    }

    /* ── Live Tab ─────────────────────────────────────────── */
    function renderMatches(data) {
        var live = [], upcoming = [], finished = [];
        (data.matches || []).forEach(function (m) {
            if (isLive(m.status))   live.push(m);
            else if (m.status === 'FT') finished.push(m);
            else if (m.status === 'NS') upcoming.push(m);
        });

        renderMatchGroup('matches-live',     live,     'No live matches right now');
        renderMatchGroup('matches-upcoming', upcoming, 'No upcoming matches loaded');
        renderMatchGroup('matches-finished', finished, 'No results yet');

        /* status row */
        var badge = document.getElementById('st-enabled');
        badge.textContent = data.enabled ? 'Enabled' : 'Disabled';
        badge.className = 'badge ' + (data.enabled ? 'on' : 'off');
        document.getElementById('st-poll').textContent = data.last_poll || '--';
        var pm = document.getElementById('st-poll-mode');
        if (pm) pm.textContent = (data.poll_mode || 'idle') + ' ' + (data.effective_poll_sec || '--') + 's';

        var sb = document.getElementById('src-badge');
        sb.textContent = data.data_source || '--';
        sb.className = 'src-badge src-' + (data.data_source || 'none').replace('-','');

        var db = document.getElementById('demo-badge');
        if (data.demo_mode) db.classList.remove('hidden');
        else                db.classList.add('hidden');
    }

    function renderMatchGroup(containerId, matches, emptyMsg) {
        var el = document.getElementById(containerId);
        if (!matches.length) {
            el.innerHTML = '<div class="empty-msg">' + esc(emptyMsg) + '</div>';
            return;
        }
        el.innerHTML = matches.map(function (m) {
            var live   = isLive(m.status);
            var ft     = m.status === 'FT';
            var cls    = live ? 'match-card live' : ft ? 'match-card finished' : 'match-card';
            var tracked = m.tracked ? ' tracked' : '';
            var stLabel = statusLabel(m.status, m.elapsed, m.extra);
            var scoreHtml = (live || ft)
                ? '<span class="mc-score">' + m.home_score + '</span>'
                  + '<span class="mc-sep">&ndash;</span>'
                  + '<span class="mc-score">' + m.away_score + '</span>'
                : '<span class="mc-score muted">&mdash;</span>'
                  + '<span class="mc-sep">vs</span>'
                  + '<span class="mc-score muted">&mdash;</span>';
            var eventHtml = '';
            if (m.last_event) {
                var evText = esc(m.last_event);
                /* Bold the scorer name: "⚽ GOAL 58' Pulisic (USA)" → bold "Pulisic" */
                evText = evText.replace(/(\d+(?:\+\d+)?') ([^(]+) \(/, function (_, min, name) {
                    return min + ' <strong>' + name.trim() + '</strong> (';
                });
                eventHtml = '<div class="mc-event">' + evText + '</div>';
            }
            var koHtml = (!live && !ft && m.kickoff)
                ? '<div class="mc-kickoff">' + esc(m.kickoff.replace('T',' ').replace('Z',' UTC').substring(0,19)) + '</div>' : '';
            return '<div class="' + cls + tracked + '">'
                + '<div class="mc-header">'
                +   '<span class="mc-group">' + esc(m.group) + '</span>'
                +   '<span class="mc-clock ' + (live?'live-clock':'') + '">' + esc(stLabel) + '</span>'
                + '</div>'
                + '<div class="mc-body">'
                +   '<div class="mc-team home">'
                +     '<span class="mc-flag">' + (m.home_flag||'') + '</span>'
                +     '<span class="mc-code">' + esc(m.home_code) + '</span>'
                +   '</div>'
                +   '<div class="mc-scores">' + scoreHtml + '</div>'
                +   '<div class="mc-team away">'
                +     '<span class="mc-code">' + esc(m.away_code) + '</span>'
                +     '<span class="mc-flag">' + (m.away_flag||'') + '</span>'
                +   '</div>'
                + '</div>'
                + eventHtml + koHtml
                + '</div>';
        }).join('');
    }

    function refreshStatus() {
        api('/status').then(function (r) { return r.json(); }).then(function (d) {
            renderMatches(d);
        }).catch(function (e) {
            document.getElementById('matches-live').innerHTML =
                '<div class="empty-msg error">Failed to reach app backend: ' + e + '</div>';
        });
    }

    refreshStatus();
    setInterval(refreshStatus, 15000);
    document.getElementById('btn-refresh').addEventListener('click', function () {
        api('/refresh', { method: 'POST' }).catch(function () {});
        setTimeout(refreshStatus, 1500);
    });

    /* ── Standings Tab ────────────────────────────────────── */
    function refreshStandings() {
        api('/standings').then(function (r) { return r.json(); }).then(function (d) {
            var el = document.getElementById('standings-grid');
            var groups = d.groups || [];
            if (!groups.length) {
                el.innerHTML = '<div class="empty-msg">No standings data yet. Trigger a refresh or check Demo Mode.</div>';
                return;
            }
            el.innerHTML = groups.map(function (g) {
                var rows = (g.table || []).map(function (t, i) {
                    var tr = t.tracked ? ' class="tracked-row"' : '';
                    var gd = t.gd >= 0 ? '+' + t.gd : String(t.gd);
                    return '<tr' + tr + '>'
                        + '<td>' + t.rank + '</td>'
                        + '<td class="flag-cell">' + (t.flag||'') + '</td>'
                        + '<td class="code-cell">' + esc(t.code) + '</td>'
                        + '<td>' + t.played + '</td>'
                        + '<td>' + t.won + '</td>'
                        + '<td>' + t.drawn + '</td>'
                        + '<td>' + t.lost + '</td>'
                        + '<td>' + t.gf + '</td>'
                        + '<td>' + t.ga + '</td>'
                        + '<td class="gd-cell">' + gd + '</td>'
                        + '<td class="pts-cell">' + t.pts + '</td>'
                        + '</tr>';
                }).join('');
                return '<div class="standing-group">'
                    + '<div class="standing-group-title">Group ' + esc(g.group) + '</div>'
                    + '<table class="standing-table">'
                    + '<thead><tr><th>#</th><th></th><th>Team</th>'
                    + '<th>P</th><th>W</th><th>D</th><th>L</th>'
                    + '<th>GF</th><th>GA</th><th>GD</th><th>Pts</th></tr></thead>'
                    + '<tbody>' + rows + '</tbody>'
                    + '</table></div>';
            }).join('');
        }).catch(function () {
            document.getElementById('standings-grid').innerHTML =
                '<div class="empty-msg error">Could not load standings.</div>';
        });
    }

    /* ── Bracket Tab ─────────────────────────────────────── */
    function refreshBracket() {
        api('/bracket').then(function (r) { return r.json(); }).then(function (d) {
            var el = document.getElementById('bracket-grid');
            var rounds = d.rounds || [];
            if (!rounds.length) {
                el.innerHTML = '<div class="empty-msg">No knockout matches yet. Check back after the group stage.</div>';
                return;
            }
            el.innerHTML = rounds.map(function (r) {
                var cards = (r.matches || []).map(function (m) {
                    var live     = isLive(m.status);
                    var ft       = m.status === 'FT';
                    var cls      = live ? 'match-card live' : ft ? 'match-card finished' : 'match-card';
                    var tracked  = m.tracked ? ' tracked' : '';
                    var stLabel  = statusLabel(m.status, m.elapsed);
                    var scoreHtml = (live || ft)
                        ? '<span class="mc-score">' + m.home_score + '</span>'
                          + '<span class="mc-sep">&ndash;</span>'
                          + '<span class="mc-score">' + m.away_score + '</span>'
                        : '<span class="mc-score muted">&mdash;</span>'
                          + '<span class="mc-sep">vs</span>'
                          + '<span class="mc-score muted">&mdash;</span>';
                    var eventHtml = '';
                    if (m.last_event) {
                        var evText = esc(m.last_event);
                        evText = evText.replace(/(\d+(?:\+\d+)?') ([^(]+) \(/, function (_, min, name) {
                            return min + ' <strong>' + name.trim() + '</strong> (';
                        });
                        eventHtml = '<div class="mc-event">' + evText + '</div>';
                    }
                    var koHtml = (!live && !ft && m.kickoff)
                        ? '<div class="mc-kickoff">' + esc(m.kickoff.replace('T',' ').replace('Z',' UTC').substring(0,19)) + '</div>' : '';
                    return '<div class="' + cls + tracked + '">'
                        + '<div class="mc-header">'
                        +   '<span class="mc-group">' + esc(m.round) + '</span>'
                        +   '<span class="mc-clock ' + (live ? 'live-clock' : '') + '">' + esc(stLabel) + '</span>'
                        + '</div>'
                        + '<div class="mc-body">'
                        +   '<div class="mc-team home"><span class="mc-flag">' + (m.home_flag||'') + '</span><span class="mc-code">' + esc(m.home_code) + '</span></div>'
                        +   '<div class="mc-scores">' + scoreHtml + '</div>'
                        +   '<div class="mc-team away"><span class="mc-code">' + esc(m.away_code) + '</span><span class="mc-flag">' + (m.away_flag||'') + '</span></div>'
                        + '</div>'
                        + eventHtml + koHtml
                        + '</div>';
                }).join('');
                return '<div class="bracket-round">'
                    + '<div class="section-label">' + esc(r.name) + '</div>'
                    + cards
                    + '</div>';
            }).join('');
        }).catch(function () {
            document.getElementById('bracket-grid').innerHTML =
                '<div class="empty-msg error">Could not load bracket.</div>';
        });
    }

    /* ── Golden Boot Tab ──────────────────────────────────── */
    function refreshScorers() {
        api('/scorers').then(function (r) { return r.json(); }).then(function (d) {
            var el = document.getElementById('scorers-list');
            var sc = d.scorers || [];
            if (!sc.length) {
                el.innerHTML = '<div class="empty-msg">No scorer data yet.</div>';
                return;
            }
            el.innerHTML = sc.map(function (s) {
                var medal = s.rank === 1 ? '&#127945;' : s.rank === 2 ? '&#129352;' : s.rank === 3 ? '&#129353;' : String(s.rank);
                return '<div class="scorer-row">'
                    + '<span class="scorer-rank">' + medal + '</span>'
                    + '<span class="scorer-flag">' + (s.flag||'') + '</span>'
                    + '<span class="scorer-name">' + esc(s.player) + '</span>'
                    + '<span class="scorer-team">' + esc(s.team) + '</span>'
                    + '<span class="scorer-goals">' + s.goals + ' &#9917;</span>'
                    + '</div>';
            }).join('');
        }).catch(function () {
            document.getElementById('scorers-list').innerHTML =
                '<div class="empty-msg error">Could not load scorers.</div>';
        });
    }

    /* ── Config Tab — Team Grid ───────────────────────────── */
    var allTeams = [];
    var selectedCodes = {};

    function renderTeamGrid() {
        var el = document.getElementById('team-grid');
        if (!allTeams.length) { el.innerHTML = '<div class="empty-msg">Loading teams&hellip;</div>'; return; }

        /* Group by letter */
        var groups = {};
        allTeams.forEach(function (t) {
            if (!groups[t.group]) groups[t.group] = [];
            groups[t.group].push(t);
        });

        var letters = Object.keys(groups).sort();
        el.innerHTML = letters.map(function (g) {
            var cards = groups[g].map(function (t) {
                var sel = selectedCodes[t.code] ? ' selected' : '';
                return '<div class="team-chip' + sel + '" data-code="' + esc(t.code) + '" '
                    + 'style="--team-bg:' + t.bg + ';--team-fg:' + t.fg + '">'
                    + '<span class="chip-flag">' + (t.flag||'') + '</span>'
                    + '<span class="chip-code">' + esc(t.code) + '</span>'
                    + '</div>';
            }).join('');
            return '<div class="group-block">'
                + '<div class="group-label">Group ' + esc(g) + '</div>'
                + '<div class="chip-row">' + cards + '</div>'
                + '</div>';
        }).join('');

        el.querySelectorAll('.team-chip').forEach(function (chip) {
            chip.addEventListener('click', function () {
                var code = chip.dataset.code;
                if (selectedCodes[code]) {
                    delete selectedCodes[code];
                    chip.classList.remove('selected');
                } else {
                    selectedCodes[code] = true;
                    chip.classList.add('selected');
                }
            });
        });
    }

    function loadTeams() {
        api('/teams').then(function (r) { return r.json(); }).then(function (d) {
            allTeams = d.teams || [];
            allTeams.forEach(function (t) {
                if (t.selected) selectedCodes[t.code] = true;
            });
            renderTeamGrid();
        }).catch(function () {});
    }
    loadTeams();

    document.getElementById('btn-sel-all').addEventListener('click', function () {
        allTeams.forEach(function (t) { selectedCodes[t.code] = true; });
        renderTeamGrid(); renderTeamOverrides();
    });
    document.getElementById('btn-sel-none').addEventListener('click', function () {
        selectedCodes = {};
        renderTeamGrid(); renderTeamOverrides();
    });

    /* ── Config Tab — Load / Save ─────────────────────────── */
    document.getElementById('scroll-speed').addEventListener('input', function () {
        document.getElementById('scroll-speed-val').textContent = this.value;
    });
    document.getElementById('audio-volume').addEventListener('input', function () {
        document.getElementById('audio-volume-val').textContent = this.value;
    });

    var teamOverrides = {}; /* code → { clip_id, flashes } */
    var clipList = [];     /* [{id, name}] populated by loadClips() */

    /* Fetch clip list from device, then call cb(). */
    function loadClips(cb) {
        api('/clips').then(function (r) { return r.json(); }).then(function (d) {
            clipList = d.clips || [];
            if (cb) cb();
        }).catch(function () { if (cb) cb(); });
    }

    /* Returns a <select> DOM element pre-selected to selectedId.
       withGlobal=true prepends a "Use global (#0)" option. */
    function buildClipSelect(selectedId, withGlobal) {
        var sel = document.createElement('select');
        sel.className = 'form-control';
        if (withGlobal) {
            var gopt = document.createElement('option');
            gopt.value = '0';
            gopt.textContent = 'Use global (#0)';
            if (!selectedId) gopt.selected = true;
            sel.appendChild(gopt);
        }
        clipList.forEach(function (cl) {
            var opt = document.createElement('option');
            opt.value = String(cl.id);
            opt.textContent = cl.name + ' (#' + cl.id + ')';
            if (cl.id === selectedId) opt.selected = true;
            sel.appendChild(opt);
        });
        return sel;
    }

    /* Returns an HTML string <select> for use inside innerHTML templates.
       withGlobal=true prepends a "Use global (#0)" option. */
    function clipSelectHTML(selectedId, cls, code, withGlobal) {
        var opts = '';
        if (withGlobal) {
            opts += '<option value="0"' + (!selectedId ? ' selected' : '') + '>Use global (#0)</option>';
        }
        clipList.forEach(function (cl) {
            opts += '<option value="' + cl.id + '"' + (cl.id === selectedId ? ' selected' : '') + '>'
                  + esc(cl.name) + ' (#' + cl.id + ')</option>';
        });
        return '<select class="' + cls + ' form-control" data-code="' + esc(code) + '">' + opts + '</select>';
    }

    function renderTeamOverrides() {
        var el = document.getElementById('team-overrides-grid');
        var codes = Object.keys(selectedCodes);
        if (!codes.length) {
            el.innerHTML = '<p class="hint">No teams selected — pick teams above first.</p>';
            return;
        }
        el.innerHTML = '<table class="diag-table" style="width:100%">'
            + '<thead><tr><th></th><th>Team</th><th>Clip <span class="hint-inline">(0=global)</span></th><th>Flashes <span class="hint-inline">(0=global)</span></th></tr></thead>'
            + '<tbody>'
            + codes.map(function (code) {
                var t = allTeams.find(function (t) { return t.code === code; }) || {};
                var ov = teamOverrides[code] || { clip_id: 0, flashes: 0 };
                return '<tr>'
                    + '<td>' + (t.flag || '') + '</td>'
                    + '<td><strong>' + esc(code) + '</strong></td>'
                    + '<td>' + clipSelectHTML(ov.clip_id, 'ov-clip', code, true) + '</td>'
                    + '<td><input type="number" class="ov-flash" data-code="' + esc(code) + '" min="0" max="10" value="' + ov.flashes + '" style="width:56px"></td>'
                    + '</tr>';
            }).join('')
            + '</tbody></table>';
    }

    function loadConfig() {
        api('/config').then(function (r) { return r.json(); }).then(function (d) {
            document.getElementById('enabled').checked      = !!d.enabled;
            document.getElementById('disp-enabled').checked = !!d.display_enabled;
            document.getElementById('demo-mode').checked    = !!d.demo_mode;
            document.getElementById('device-user').value    = d.device_user || 'root';
            document.getElementById('poll-interval').value  = String(d.poll_interval_sec || 900);
            document.getElementById('text-color').value     = d.text_color  || '#FFFFFF';
            document.getElementById('bg-color').value       = d.bg_color    || '#003F7F';
            document.getElementById('text-size').value      = d.text_size   || 'large';
            document.getElementById('scroll-speed').value   = String(d.scroll_speed || 3);
            document.getElementById('scroll-speed-val').textContent = String(d.scroll_speed || 3);
            document.getElementById('duration-sec').value  = String(Math.round((d.duration_ms||15000)/1000));
            document.getElementById('live-poll-sec').value     = String(d.live_poll_sec    || 30);
            document.getElementById('prematch-poll-sec').value = String(d.prematch_poll_sec || 90);
            document.getElementById('idle-poll-sec').value     = String(d.idle_poll_sec     || 300);
            document.getElementById('poll-interval').value     = String(d.poll_interval_sec || 0);
            document.getElementById('webhook-enabled').checked = !!d.webhook_enabled;
            document.getElementById('webhook-url').placeholder =
                d.webhook_url === '***' ? '(configured — paste to update)' : 'https://\u2026';
            document.getElementById('strobe-enabled').checked = (d.strobe_enabled !== false);
            document.getElementById('strobe-flashes').value  = String(d.strobe_flashes != null ? d.strobe_flashes : 5);
            document.getElementById('audio-enabled').checked = (d.audio_enabled !== false);
            document.getElementById('audio-volume').value    = String(d.audio_volume != null ? d.audio_volume : 75);
            document.getElementById('audio-volume-val').textContent = String(d.audio_volume != null ? d.audio_volume : 75);

            /* Inject clip dropdowns populated from device clip list */
            var goalSel = buildClipSelect(d.goal_clip_id || 1, false);
            goalSel.id  = 'goal-clip-id';
            var gWrap   = document.getElementById('goal-clip-wrap');
            gWrap.innerHTML = ''; gWrap.appendChild(goalSel);

            var alertSel = buildClipSelect(d.alert_clip_id || 2, false);
            alertSel.id  = 'alert-clip-id';
            var aWrap    = document.getElementById('alert-clip-wrap');
            aWrap.innerHTML = ''; aWrap.appendChild(alertSel);

            /* Rebuild selection from config */
            selectedCodes = {};
            (d.selected_teams || []).forEach(function (c) { selectedCodes[c] = true; });
            renderTeamGrid();

            /* Per-team overrides */
            teamOverrides = {};
            (d.team_overrides || []).forEach(function (ov) {
                teamOverrides[ov.code] = { clip_id: ov.clip_id || 0, flashes: ov.flashes || 0 };
            });
            renderTeamOverrides();

            /* api key hint */
            document.getElementById('af-key').placeholder =
                d.af_key === '***' ? '(key saved — paste to update)' : 'Paste key here\u2026';
            document.getElementById('fd-key').placeholder =
                d.fd_key === '***' ? '(key saved — paste to update)' : 'Paste key here\u2026';
        }).catch(function () {});
    }
    loadClips(loadConfig);

    function showMsg(txt, ok) {
        var el = document.getElementById('save-msg');
        el.textContent = txt;
        el.className = 'save-msg ' + (ok === false ? 'error' : '');
        setTimeout(function () { el.textContent = ''; el.className = 'save-msg'; }, 3000);
    }

    document.getElementById('btn-save').addEventListener('click', function () {
        var payload = {
            selected_teams:    Object.keys(selectedCodes),
            enabled:           document.getElementById('enabled').checked,
            display_enabled:   document.getElementById('disp-enabled').checked,
            demo_mode:         document.getElementById('demo-mode').checked,
            device_user:       document.getElementById('device-user').value,
            poll_interval_sec:  parseInt(document.getElementById('poll-interval').value, 10) || 0,
            live_poll_sec:      parseInt(document.getElementById('live-poll-sec').value, 10),
            prematch_poll_sec:  parseInt(document.getElementById('prematch-poll-sec').value, 10),
            idle_poll_sec:      parseInt(document.getElementById('idle-poll-sec').value, 10),
            webhook_enabled:    document.getElementById('webhook-enabled').checked,
            text_color:        document.getElementById('text-color').value,
            bg_color:          document.getElementById('bg-color').value,
            text_size:         document.getElementById('text-size').value,
            scroll_speed:      parseInt(document.getElementById('scroll-speed').value, 10),
            duration_ms:       parseInt(document.getElementById('duration-sec').value, 10) * 1000,
            strobe_enabled:    document.getElementById('strobe-enabled').checked,
            strobe_flashes:    parseInt(document.getElementById('strobe-flashes').value, 10),
            audio_enabled:     document.getElementById('audio-enabled').checked,
            audio_volume:      parseInt(document.getElementById('audio-volume').value, 10),
            goal_clip_id:      parseInt(document.getElementById('goal-clip-id').value, 10),
            alert_clip_id:     parseInt(document.getElementById('alert-clip-id').value, 10),
        };
        /* Collect per-team overrides from the rendered table */
        var overridesArr = [];
        document.querySelectorAll('#team-overrides-grid .ov-clip').forEach(function (inp) {
            var code    = inp.dataset.code;
            var clipId  = parseInt(inp.value, 10) || 0;
            var flashEl = document.querySelector('#team-overrides-grid .ov-flash[data-code="' + code + '"]');
            var flashes = flashEl ? (parseInt(flashEl.value, 10) || 0) : 0;
            overridesArr.push({ code: code, clip_id: clipId, flashes: flashes });
        });
        payload.team_overrides = overridesArr;

        var afKey      = document.getElementById('af-key').value.trim();
        var fdKey      = document.getElementById('fd-key').value.trim();
        var pass       = document.getElementById('device-pass').value;
        var webhookUrl = document.getElementById('webhook-url').value.trim();
        if (afKey)      payload.af_key      = afKey;
        if (fdKey)      payload.fd_key      = fdKey;
        if (pass)       payload.device_pass = pass;
        if (webhookUrl) payload.webhook_url = webhookUrl;

        api('/config', {
            method:  'POST',
            headers: { 'Content-Type': 'application/json' },
            body:    JSON.stringify(payload),
        }).then(function (r) { return r.json(); })
          .then(function (d) {
              showMsg(d.message || 'Saved', true);
              document.getElementById('device-pass').value = '';
              document.getElementById('af-key').value = '';
              document.getElementById('fd-key').value = '';
              document.getElementById('webhook-url').value = '';
              loadConfig();
          })
          .catch(function (e) { showMsg('Save failed: ' + e, false); });
    });

    document.getElementById('btn-test-display').addEventListener('click', function () {
        api('/test_display', { method: 'POST' })
            .then(function (r) { return r.json(); })
            .then(function (d) { showMsg(d.message || 'Sent'); })
            .catch(function () { showMsg('Display test failed', false); });
    });

    /* Webhook test */
    document.getElementById('btn-test-webhook').addEventListener('click', function () {
        var el = document.getElementById('webhook-msg');
        el.textContent = 'Sending test payload\u2026'; el.className = 'save-msg';
        api('/test_webhook', { method: 'POST' })
            .then(function (r) { return r.json(); })
            .then(function (d) {
                var ok = (d.http_code >= 200 && d.http_code < 300);
                el.textContent = (ok ? '\u2713' : '\u2717') + ' HTTP ' + d.http_code +
                    (d.response ? ' | ' + d.response : '') +
                    (!ok ? '\n' + (d.curl_error || '') : '');
                el.className = 'save-msg' + (ok ? '' : ' error');
                setTimeout(function () { el.textContent = ''; el.className = 'save-msg'; }, 5000);
            })
            .catch(function (e) {
                el.textContent = '\u2717 ' + e; el.className = 'save-msg error';
            });
    });

    function showAudioMsg(txt, ok) {
        var el = document.getElementById('audio-msg');
        el.textContent = txt;
        el.className = 'save-msg ' + (ok === false ? 'error' : '');
        setTimeout(function () { el.textContent = ''; el.className = 'save-msg'; }, 5000);
    }

    document.getElementById('btn-upload-clips').addEventListener('click', function () {
        showAudioMsg('Uploading clips to device\u2026');
        api('/upload_clips', { method: 'POST' })
            .then(function (r) { return r.json(); })
            .then(function (d) {
                var clips = d.clips || [];
                var lines = clips.map(function (c) {
                    var ok = (c.http_code >= 200 && c.http_code < 300);
                    return (ok ? '\u2713' : '\u2717') + ' ' + c.name
                        + ' \u2192 HTTP ' + c.http_code
                        + (c.response ? ' | ' + c.response : '')
                        + (c.file_exists === 'no' ? ' [FILE NOT FOUND]' : '');
                });
                var allOk = clips.every(function (c) {
                    return c.http_code >= 200 && c.http_code < 300;
                });
                showAudioMsg(lines.join('\n') || 'Done', allOk);
            })
            .catch(function (e) { showAudioMsg('Upload failed: ' + e, false); });
    });

    document.getElementById('btn-test-audio').addEventListener('click', function () {
        showAudioMsg('Playing goal sound\u2026');
        api('/test_audio', { method: 'POST' })
            .then(function (r) { return r.json(); })
            .then(function (d) {
                var ok = (d.clip_http >= 200 && d.clip_http < 300);
                showAudioMsg(
                    (ok ? '\u2713' : '\u2717') +
                    ' clip=' + d.clip_id +
                    ' vol=' + d.volume + '% HTTP ' + d.clip_http +
                    (d.clip_resp ? '\n' + d.clip_resp : ''), ok);
            })
            .catch(function (e) { showAudioMsg('Audio test failed: ' + e, false); });
    });

    /* ── Diagnostics Tab ──────────────────────────────────── */
    function diagRun(btnId, outId, action) {
        var btn = document.getElementById(btnId);
        var out = document.getElementById(outId);
        btn.disabled = true;
        out.textContent = 'Running\u2026';
        action(function (text, ok) {
            out.textContent = text;
            out.className = 'diag-out' + (ok === false ? ' diag-err' : ' diag-ok');
            btn.disabled = false;
        });
    }

    /* 1 — Ping */
    document.getElementById('diag-ping').addEventListener('click', function () {
        diagRun('diag-ping', 'diag-ping-out', function (done) {
            var t0 = Date.now();
            api('/status')
                .then(function (r) { return r.json(); })
                .then(function (d) {
                    done('✓ App responded in ' + (Date.now()-t0) + ' ms\n'
                        + 'enabled=' + d.enabled + '  demo=' + d.demo_mode
                        + '  source=' + (d.data_source||'none')
                        + '\nlast_poll=' + (d.last_poll||'never')
                        + '\nmatches cached: ' + (d.matches||[]).length
                        + '  scorers: ' + (d.top_scorers||[]).length, true);
                })
                .catch(function (e) {
                    done('✗ Failed: ' + e + '\n\nCheck:\n'
                        + '  • ACAP app is installed and running\n'
                        + '  • Reverse proxy in manifest.json maps /api → port 2016\n'
                        + '  • No firewall blocking loopback on device', false);
                });
        });
    });

    /* 2 — Mock data */
    document.getElementById('diag-mock').addEventListener('click', function () {
        diagRun('diag-mock', 'diag-mock-out', function (done) {
            /* Force demo mode refresh */
            api('/refresh', { method: 'POST' })
                .then(function () { return new Promise(function (r) { setTimeout(r, 2000); }); })
                .then(function () { return api('/status'); })
                .then(function (r) { return r.json(); })
                .then(function (d) {
                    var nm = (d.matches||[]).length;
                    var ns = (d.top_scorers||[]).length;
                    if (nm > 0 || ns > 0) {
                        done('✓ Mock data loaded\n'
                            + 'Matches: ' + nm + '  Scorers: ' + ns + '\n'
                            + 'Source: ' + (d.data_source||'?'), true);
                    } else {
                        done('✗ No data returned after refresh.\n\n'
                            + 'Check:\n'
                            + '  • Demo Mode is ON in Config\n'
                            + '  • mock/*.json files are packaged in the .eap\n'
                            + '    (look for them at /usr/local/packages/fifa_wc/mock/)\n'
                            + '  • Rebuild with: docker build --tag fifa_wc:1.0.0 .\n'
                            + '    and reinstall the .eap', false);
                    }
                })
                .catch(function (e) { done('✗ ' + e, false); });
        });
    });

    /* 3 — Force refresh */
    document.getElementById('diag-refresh').addEventListener('click', function () {
        diagRun('diag-refresh', 'diag-refresh-out', function (done) {
            api('/refresh', { method: 'POST' })
                .then(function (r) { return r.json(); })
                .then(function (d) {
                    done('✓ ' + (d.message||'Refresh queued') + '\n\n'
                        + 'Wait ~5 s then check the Live tab.\n'
                        + 'If no data appears and Demo Mode is OFF:\n'
                        + '  • Verify API keys are entered in Config\n'
                        + '  • Check free tier quota (api-football: 100 req/day)\n'
                        + '  • Enable Demo Mode to bypass API limits', true);
                })
                .catch(function (e) { done('✗ ' + e, false); });
        });
    });

    /* 4 — Display test */
    document.getElementById('diag-display').addEventListener('click', function () {
        diagRun('diag-display', 'diag-display-out', function (done) {
            api('/test_display', { method: 'POST' })
                .then(function (r) { return r.json(); })
                .then(function (d) {
                    var http = d.display_http;
                    var ok   = (http >= 200 && http < 300);
                    var hint = ok ? '' :
                        '\n\nIf nothing appears on the C1720 display:\n'
                        + '  • Check device username/password in Config\n'
                        + '  • Confirm "Show on Display" is ON\n'
                        + '  • Verify the VAPIX display endpoint is reachable\n'
                        + '    (device must be a C1720 / C1710)';
                    done((ok ? '✓' : '✗') + ' display API HTTP ' + http + '\n'
                        + (d.display_resp || '') + hint, ok);
                })
                .catch(function (e) { done('✗ ' + e, false); });
        });
    });

    /* 5 — Audio pipeline */
    document.getElementById('diag-audio').addEventListener('click', function () {
        diagRun('diag-audio', 'diag-audio-out', function (done) {
            api('/test_audio', { method: 'POST' })
                .then(function (r) { return r.json(); })
                .then(function (d) {
                    var ok = (d.clip_http >= 200 && d.clip_http < 300);
                    done((ok ? '\u2713' : '\u2717') +
                        ' clip=' + d.clip_id +
                        ' vol=' + d.volume + '% HTTP ' + d.clip_http +
                        (d.clip_resp ? '\n' + d.clip_resp : '') +
                        (ok ? '' :
                            '\n\nIf no sound:\n' +
                            '  \u2022 Run "Upload Clips to Device" in Config first\n' +
                            '  \u2022 Verify device credentials in Config\n' +
                            '  \u2022 Goal Clip ID in Config matches the uploaded clip\n' +
                            '  \u2022 Volume > 0 in Config'), ok);
                })
                .catch(function (e) { done('\u2717 ' + e, false); });
        });
    });

    /* 6 — Full dump */
    document.getElementById('diag-dump').addEventListener('click', function () {
        diagRun('diag-dump', 'diag-dump-out', function (done) {
            api('/status')
                .then(function (r) { return r.json(); })
                .then(function (d) { done(JSON.stringify(d, null, 2), true); })
                .catch(function (e) { done('✗ ' + e, false); });
        });
    });

})();
