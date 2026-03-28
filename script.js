let username = localStorage.getItem("chat_name");
let userColor = localStorage.getItem("chat_color");

if (username == null || username.length === 0) {
  window.location.href = `/name.html?v=${Date.now()}`;
  throw new Error("Missing username; redirecting to name page.");
}

if (typeof userColor !== "string" || !/^#[0-9a-fA-F]{6}$/.test(userColor)) {
  userColor = null;
}

let username_ok = false;
let msgBox = document.getElementById('msgs-box');
let input = document.getElementById('msg-input');
let users = new Map();
let users_typing = Array();
let typing = false;
let typing_t = false;
const typing_timer = 500;
const outlinedChatTextStrokeWidth = "0.0001px";

input.addEventListener('blur', () => input.focus());
input.focus();

const wsProtocol = window.location.protocol === "https:" ? "wss:" : "ws:";
const wsHost = "chat.waffledogz.us"; // Change this to your server's address and port if needed
const socket = new WebSocket(`${wsProtocol}//${wsHost}/ws`);
let movedToErrorPage = false;

socket.addEventListener("error", () => {
  addMessage("an error occurred. check console for details", "red");
});

socket.addEventListener("open", () => {
  socket.send(
    "&u" +
      JSON.stringify({
        username,
        color: userColor,
      })
  );
});

const notifAudio = new Audio("/notif.mp3");
let audioUnlocked = false;

document.addEventListener("keydown", () => {
  if (!audioUnlocked) {
    notifAudio.play().then(()=>{notifAudio.pause();notifAudio.currentTime=0;});audioUnlocked=true;}});

function playNotificationBeep() {
  notifAudio.currentTime = 0;
  notifAudio.play().catch(()=>{});
}

function addMessage(message, color) {
  playNotificationBeep();
  addMessageAt(message, color, Date.now());
}

function formatTimestamp(timestamp) {
  return new Date(timestamp).toLocaleTimeString([], {hour: '2-digit', minute: '2-digit'});
}

function shouldOutlineChatName(color) {
  if (typeof color !== "string") return false;
  const normalizedColor = color.replace(/\s+/g, "").toLowerCase();
  return normalizedColor === "rgb(35,42,46)" || normalizedColor === "#232a2e";
}

function applyOutlinedChatTextStyles(element) {
  element.style.webkitTextStroke = `${outlinedChatTextStrokeWidth} white`;
  element.style.textStroke = `${outlinedChatTextStrokeWidth} white`;
  element.style.textShadow = "-1px 0 white, 0 1px white, 1px 0 white, 0 -1px white";
}

function addMessageAt(message, color, timestamp) {
  let txt = document.createElement("p");
  let time = formatTimestamp(timestamp);
  let timespan = document.createElement("span");
  timespan.innerHTML = `[${time}]&emsp;`; 
  timespan.style.cssText = "color: var(--border); font-size: 12px;";
  let msgspan = document.createElement("span");
  msgspan.textContent = message;
  msgspan.style.color = color;
  txt.appendChild(timespan);
  txt.appendChild(msgspan);
  txt.style.color = color;
  txt.className = "chat-msg";
  msgBox.prepend(txt);
}

function addChatMessageAt(username, message, color, timestamp) {
  let txt = document.createElement("p");
  let time = formatTimestamp(timestamp);
  let timespan = document.createElement("span");
  let namespan = document.createElement("span");
  let msgspan = document.createElement("span");

  timespan.innerHTML = `[${time}]&emsp;`;
  timespan.style.cssText = "color: var(--border); font-size: 12px;";

  namespan.textContent = `<${username}> `;
  namespan.style.color = color;
  if (shouldOutlineChatName(color)) {
    applyOutlinedChatTextStyles(namespan);
  }

  msgspan.textContent = message;
  msgspan.style.color = color;
  if (shouldOutlineChatName(color)) {
    applyOutlinedChatTextStyles(msgspan);
  }

  txt.appendChild(timespan);
  txt.appendChild(namespan);
  txt.appendChild(msgspan);
  txt.style.color = color;
  txt.className = "chat-msg";
  msgBox.prepend(txt);
}

function addUser(id, user) {
	users.set(id,{username: user.username, color: user.color});
	let txt = document.createElement("p");
	txt.textContent = user.username;
	txt.id = "s-u-" + id;
	txt.style.color = users.get(id).color;
	document.getElementById("users-box").appendChild(txt);
}

let has_gotten_users = false;

socket.addEventListener("message", (event) => {
  let d = event.data;
  let dj = JSON.parse(d)

  let event_type = dj.event;

  if (!username_ok) {
    if (event_type == "uname-eval") {
      if (dj.result == "ok") {
	username_ok = true;
      }
      else if (dj.result == "taken") {
        alert("username taken :(");
        localStorage.removeItem("chat_name");
        window.location.href = `/name.html?v=${Date.now()}`;
      }
    }
    return;
  }
  if (!has_gotten_users) {
    socket.send("&i");
    has_gotten_users = true;
  }

  switch (event_type) {
    case "sendusers": {
      for (let [id,user] of Object.entries(dj.users)) {
	addUser(id,user);
      }
      return;
    }
    case "userjoin": {
      addUser(dj.id, {username: dj.username, color: dj.color});
      addMessageAt(`<${users.get(dj.id).username}> joined.`, users.get(dj.id).color, dj.timestamp ?? Date.now())
      return;
    }
    case "userleft": {
      document.getElementById("s-u-" + dj.id).remove();
      let txt = document.createElement("p");
      addMessageAt(`<${users.get(dj.id).username}> left.`,users.get(dj.id).color, dj.timestamp ?? Date.now())

      users_typing.splice(users_typing.indexOf(dj.id),1);
      users.delete(dj.id);
      return;
    }
    case "msg": {
      const user = users.get(dj.id);
      const messageUsername = dj.username ?? user?.username ?? "unknown";
      const messageColor = dj.color ?? user?.color ?? "white";
      addChatMessageAt(messageUsername, dj.msg, messageColor, dj.timestamp ?? Date.now());
      return;
    }
    case "typing": {
      users_typing.push(dj.id);
      update_typing_span();
      console.log(users_typing);
      return;
    }
    case "stoptyping": {
      users_typing.splice(users_typing.indexOf(dj.id), 1);
      update_typing_span();
      console.log(users_typing);
      return;
    }
  }
});

let typing_reset_timeout;

function send(event) {
  if (event.keyCode != 13) {
  	typing = true;
 	 clearTimeout(typing_reset_timeout);
 	 typing_reset_timeout = setTimeout(()=>{typing=false},typing_timer);
  }
  if (event && event.keyCode != 13) return;
  
  let input_txt = input.value;
  if (input_txt.trim() == "") return;
  socket.send(input_txt);
  input.value = "";
  input.focus();
}

function server_update_typing() {
  if (typing != typing_t) {
    if (typing) {
      socket.send("&t");
    }
    else {
      socket.send("&s");
    }
    typing_t = typing;
  }
}

function update_typing_span() {
  let spans = document.createElement("span");
  spans.id = "us-typing";
  spans.textContent = "Typing:"
  users_typing.forEach((id) => {
   let s = document.createElement("span");
   s.textContent = `<${users.get(id).username}>`;
   s.style.color = users.get(id).color; 
   s.className = "u-typing-span"
    spans.appendChild(s);
  })
  document.getElementById("us-typing").replaceWith(spans);
}

setInterval(server_update_typing, 30);
