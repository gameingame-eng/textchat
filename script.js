let username = prompt("Username: ");
let msgBox = document.getElementById('msgs-box');
let input = document.getElementById('msg-input');

input.addEventListener('blur', () => input.focus());
input.focus();

const socket = new WebSocket("wss://chat.waffledogz.us/ws");

socket.addEventListener("error", (e) => alert("WS error: " + e));
socket.addEventListener("close", (e) => alert("WS closed: " + e.code + " " + e.reason));

socket.addEventListener("open", () => {
  console.log(username);
  socket.send("&u" + username);
});

socket.addEventListener("message", (event) => {
  console.log(event);
  let txt = document.createElement("p");
  txt.textContent = event.data;
  msgBox.appendChild(txt);
  msgBox.scrollTop = msgBox.scrollHeight;
});

function send(event) {
  if (event && event.keyCode != 13) return;
  let input_txt = input.value;
  if (input_txt.trim() == "") return;
  socket.send("<" + username + "> " + input_txt);
  input.value = "";
  input.focus();
}
