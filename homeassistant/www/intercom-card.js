// Browser-based intercom pickup card.
//
// Audio format expected by the gateway: 8 kHz mono int16 PCM, exactly 160
// samples (320 bytes, 20 ms) per WebSocket message — anything else is dropped
// by server.py, so the microphone stream is resampled and re-framed here.
//
// Browsers only expose the microphone on secure origins: open Home Assistant
// over HTTPS and point `gateway_url` at a wss:// endpoint (see SETUP.md).
const RATE = 8000, FRAME = 160;

class IntercomCard extends HTMLElement {
  setConfig(config) {
    this.config = config || {};
    this.ws = this.ctx = this.stream = this.proc = null;
    this.pending = []; this.error = '';
  }
  set hass(hass) {
    const old = this._hass?.states['input_select.intercom_state']?.state;
    this._hass = hass;
    const st = this.state();
    if (st !== old || !this.innerHTML) this.render();
    // Call ended elsewhere (timeout, another device, hardware button): release the mic.
    if (st === 'idle' && this.ws) this.stopAudio();
  }
  getCardSize() { return 3; }
  state() { return this._hass?.states['input_select.intercom_state']?.state || 'idle'; }
  call(s) { return this._hass.callService('script', s); }

  gatewayUrl() {
    // Behind a reverse proxy, set gateway_url (e.g. wss://ha.example.com/intercom-ws).
    if (this.config.gateway_url) return this.config.gateway_url;
    const https = location.protocol === 'https:';
    const host = this.config.gateway_host || location.hostname;
    const port = https ? (this.config.gateway_tls_port || 8443) : (this.config.gateway_port || 8099);
    return `${https ? 'wss' : 'ws'}://${host}:${port}/ws`;
  }

  async answer() {
    this.error = '';
    if (!window.isSecureContext || !navigator.mediaDevices?.getUserMedia) {
      this.error = 'The microphone is only available when Home Assistant is opened over HTTPS.';
      this.render(); return;
    }
    try {
      await this.connectAudio();
      await this.call('intercom_answer_digital');
    } catch (e) {
      this.stopAudio();
      this.error = 'Could not open audio: ' + (e.message || e);
    }
    this.render();
  }

  async connectAudio() {
    this.stream = await navigator.mediaDevices.getUserMedia({
      audio: { echoCancellation: true, noiseSuppression: true, autoGainControl: true }, video: false });
    // Use the device's native rate: Firefox refuses to connect a mic source to
    // an AudioContext created with a different sample rate.
    this.ctx = new (window.AudioContext || window.webkitAudioContext)();
    const src = this.ctx.createMediaStreamSource(this.stream);
    this.proc = this.ctx.createScriptProcessor(2048, 1, 1);
    const ratio = this.ctx.sampleRate / RATE;
    let pos = 0;

    this.ws = new WebSocket(this.gatewayUrl());
    this.ws.binaryType = 'arraybuffer';
    this.ws.onerror = () => { this.error = 'Cannot reach the audio gateway (' + this.gatewayUrl() + ').'; this.render(); };

    this.proc.onaudioprocess = (e) => {
      e.outputBuffer.getChannelData(0).fill(0);            // never play our own mic back
      if (!this.ws || this.ws.readyState !== 1) return;
      const input = e.inputBuffer.getChannelData(0);
      // Box-filter downsampling from the device rate to 8 kHz.
      for (; pos < input.length; pos += ratio) {
        const a = Math.floor(pos), b = Math.min(input.length, Math.floor(pos + ratio));
        let s = 0; for (let i = a; i < b; i++) s += input[i];
        this.pending.push(Math.max(-1, Math.min(1, s / Math.max(1, b - a))));
      }
      pos -= input.length;
      while (this.pending.length >= FRAME) {
        const f = this.pending.splice(0, FRAME), out = new Int16Array(FRAME);
        for (let i = 0; i < FRAME; i++) out[i] = f[i] * 32767;
        this.ws.send(out.buffer);
      }
    };
    src.connect(this.proc); this.proc.connect(this.ctx.destination);

    let playAt = 0;
    this.ws.onmessage = (ev) => {
      const s = new Int16Array(ev.data);
      const buf = this.ctx.createBuffer(1, s.length, RATE), d = buf.getChannelData(0);
      for (let i = 0; i < s.length; i++) d[i] = s[i] / 32768;
      const node = this.ctx.createBufferSource(); node.buffer = buf; node.connect(this.ctx.destination);
      // Small 60 ms jitter buffer; resync if playback drifts too far behind or ahead.
      const now = this.ctx.currentTime;
      if (playAt < now || playAt > now + 0.5) playAt = now + 0.06;
      node.start(playAt); playAt += buf.duration;
    };
  }

  stopAudio() {
    try { this.ws?.close(); } catch (e) {}
    this.proc?.disconnect();
    this.stream?.getTracks().forEach(t => t.stop());
    try { this.ctx?.close(); } catch (e) {}
    this.ws = this.proc = this.stream = this.ctx = null; this.pending = [];
  }
  async hangup() { this.stopAudio(); await this.call('intercom_hangup'); }
  async openGate() {
    if (this.config.confirm_gate !== false && !confirm('Open the gate?')) return;
    await this.call('intercom_open_gate');
  }

  render() {
    if (!this._hass) return;
    const st = this.state(), live = !!this.ws;
    const label = { idle: '🟢 Idle', ringing: '🔔 Incoming call', in_call: live ? '🎙️ In call' : '📞 In call (another device)' }[st];
    const btn = 'padding:14px 20px;font-size:16px;border-radius:10px;border:none;cursor:pointer;margin:4px';
    this.innerHTML = `<ha-card header="${this.config.title || '📞 Intercom'}"><div style="padding:16px;text-align:center">
      <div style="font-size:22px;margin-bottom:16px">${label}</div>
      ${st === 'ringing' || (st === 'in_call' && !live) ? `<button id="answer" style="${btn};background:#2e7d32;color:#fff">🎙️ Answer</button>` : ''}
      ${live ? `<button id="hang" style="${btn};background:#c62828;color:#fff">🔴 Hang Up</button>` : ''}
      <div><button id="gate" style="${btn};background:#1565c0;color:#fff">🔓 Open Gate</button></div>
      ${this.error ? `<div style="color:#c62828;margin-top:12px">${this.error}</div>` : ''}
    </div></ha-card>`;
    this.querySelector('#answer')?.addEventListener('click', () => this.answer());
    this.querySelector('#hang')?.addEventListener('click', () => this.hangup());
    this.querySelector('#gate')?.addEventListener('click', () => this.openGate());
  }
}
customElements.define('intercom-card', IntercomCard);
window.customCards = window.customCards || [];
window.customCards.push({ type: 'intercom-card', name: 'Digital Intercom', description: 'Browser-based intercom pickup' });
