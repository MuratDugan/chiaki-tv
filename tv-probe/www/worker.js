// Tek cekirdegi mesgul eden sabit is yuku. Ayni anda N tane calistirilinca
// sureler uzuyorsa cekirdekler baska bir seyle mesgul ya da kisitli demektir.
function work() {
  var t = Date.now(), x = 0, i;
  for (i = 0; i < 12000000; i++) {
    x = (x + Math.sqrt(i) * Math.sin(i)) % 1000003;
  }
  return Date.now() - t;
}
onmessage = function () {
  postMessage(work());
};
