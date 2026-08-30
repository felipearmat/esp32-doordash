class IntercomCard extends HTMLElement {
  setConfig(config) { this.config = config; this.ws=null; this.audio=null; this.stream=null; this.proc=null; }
  set hass(hass) { this._hass=hass; this.render(); }
  getCardSize(){ return 3; }
  state(){ return this._hass.states['input_select.intercom_state']?.state || 'idle'; }
  async call(service){ await this._hass.callService('script', service); }
  gatewayUrl(){
    const host = this.config.gateway_host || location.hostname;
    const proto = location.protocol === 'https:' ? 'wss' : 'ws';
    // For HA over HTTPS, put the gateway behind a TLS proxy; see SETUP.md.
    return `${proto}://${host}:${this.config.gateway_port || 8099}/ws`;
  }
  async startDigital(){
    await this.call('intercom_answer_digital');
    await this.connectAudio();
  }
  async connectAudio(){
    this.audio = new (window.AudioContext || window.webkitAudioContext)({sampleRate:8000});
    this.stream = await navigator.mediaDevices.getUserMedia({audio:{echoCancellation:true,noiseSuppression:true,autoGainControl:true},video:false});
    const src=this.audio.createMediaStreamSource(this.stream);
    this.proc=this.audio.createScriptProcessor(1024,1,1);
    this.ws=new WebSocket(this.gatewayUrl()); this.ws.binaryType='arraybuffer';
    this.proc.onaudioprocess=(e)=>{
      if(!this.ws || this.ws.readyState!==1) return;
      const f=e.inputBuffer.getChannelData(0), out=new Int16Array(f.length);
      for(let i=0;i<f.length;i++) out[i]=Math.max(-32768,Math.min(32767,f[i]*32767));
      this.ws.send(out.buffer);
    };
    src.connect(this.proc); this.proc.connect(this.audio.destination);
    let playAt=this.audio.currentTime;
    this.ws.onmessage=(ev)=>{
      const s=new Int16Array(ev.data); const b=this.audio.createBuffer(1,s.length,8000); const d=b.getChannelData(0);
      for(let i=0;i<s.length;i++) d[i]=s[i]/32768;
      const node=this.audio.createBufferSource(); node.buffer=b; node.connect(this.audio.destination);
      playAt=Math.max(playAt,this.audio.currentTime+0.02); node.start(playAt); playAt += b.duration;
    };
  }
  stopAudio(){
    if(this.ws){ try{this.ws.close();}catch(e){} }
    if(this.proc) this.proc.disconnect();
    if(this.stream) this.stream.getTracks().forEach(t=>t.stop());
    if(this.audio) this.audio.close();
    this.ws=this.proc=this.stream=this.audio=null;
  }
  async hangup(){ this.stopAudio(); await this.call('intercom_hangup'); }
  async openGate(){ await this.call('intercom_open_gate'); }
  render(){
    if(!this._hass) return; const st=this.state();
    this.innerHTML=`<ha-card header="📞 Intercom"><div style="padding:16px;text-align:center">
      <div style="font-size:20px;margin-bottom:16px">${st==='idle'?'🟢 Idle':st==='ringing'?'🔔 Incoming call':'🟢 In call'}</div>
      ${st==='ringing'?'<button id="answer" style="padding:12px 18px">🎙️ Answer on this device</button>':''}
      ${st==='in_call'?'<button id="hang" style="padding:12px 18px">🔴 Hang Up</button>':''}
      <div style="margin-top:12px">
        <button id="gate" style="padding:12px 18px">🔓 Open Gate</button>
      </div>
    </div></ha-card>`;
    this.querySelector('#answer')?.addEventListener('click',()=>this.startDigital());
    this.querySelector('#hang')?.addEventListener('click',()=>this.hangup());
    this.querySelector('#gate')?.addEventListener('click',()=>this.openGate());
  }
}
customElements.define('intercom-card', IntercomCard);
window.customCards=window.customCards||[];
window.customCards.push({type:'intercom-card',name:'Digital Intercom',description:'Browser-based intercom pickup'});
