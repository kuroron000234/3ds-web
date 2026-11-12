// すべてのワーカー（pthread）で読み込まれる。捕まえられなかった C++ 例外の種類・メッセージ・
// スタックをログに出す。これが無いと "Uncaught [object WebAssembly.Exception]" としか出ない。
if (typeof WorkerGlobalScope !== 'undefined' && self instanceof WorkerGlobalScope) {
  self.addEventListener('error', (e) => {
    const x = e.error;
    let msg = e.message;
    try {
      if (typeof WebAssembly.Exception !== 'undefined' && x instanceof WebAssembly.Exception) {
        const get = typeof getExceptionMessage === 'function' ? getExceptionMessage : Module['getExceptionMessage'];
        msg = get(x).filter(Boolean).join(': ');
      }
    } catch (er) {
      msg += '（内容を取り出せません: ' + er + '）';
    }
    const text = '[MSG] スレッドで例外: ' + msg + '\n' + ((x && x.stack) || '');
    try { err(text); } catch { console.error(text); }
  });
}
