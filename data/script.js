const button = document.getElementById('scan_button');
const gateway = "ws://" + window.location.hostname + "/ws";
const error_list = document.getElementById('error_list');
const vin = document.getElementById('vin-number');
const delbutton = document.getElementById('delete_button');

let lasterrors;
let websocket;
let pingTimer;
let database = {};

window.addEventListener('load', initWebSocket);
button.addEventListener('click', is_clicked);
delbutton.addEventListener('click', clear_clicked);

function initWebSocket() {
    websocket = new WebSocket(gateway);
    websocket.onopen = onOpen;
    websocket.onclose = onClose;
    websocket.onmessage = onMessage;
}
function forceDisconnect() {
    websocket.close();
}
async function loadDTCDatabase() {
    try {
        const response = await fetch('dtc_database.json');
        database = await response.json();
    } catch (error) {
        console.error("Не удалось загрузить базу данных dtc");
    }
}
window.addEventListener('load', loadDTCDatabase);
function is_clicked() {
    button.innerHTML = "Сканирование...";
    error_list.innerHTML = "Ожидайте...";
    button.disabled = true;
    delbutton.disabled = true;
    websocket.send("GET_DTC");
}
function clear_clicked() {
    delbutton.innerHTML = "Сброс...";
    error_list.innerHTML = "Ожидайте...";
    delbutton.disabled = true;
    button.disabled = true;
    websocket.send("CLEAR_DTC");
}
function onOpen(event) {
    pingTimer = setTimeout(forceDisconnect, 20000);
    const statusText = document.getElementById('status');
    statusText.innerHTML = 'Connected';
    statusText.className = "connected";
    websocket.send("GET_VIN");
}

function onClose(event) {
    const statusText = document.getElementById('status');
    statusText.innerHTML = 'Disconnected';
    statusText.className = "disconnected";
    setTimeout(initWebSocket, 2000);
}

function onMessage(event) {
    let data = JSON.parse(event.data);
    let rawText = "";
    let statusClass = "";

    if (data.active || data.pending || data.cleared || data.dtc) {
        if (data.active) {
            rawText = data.active;
            lasterrors = rawText;
            statusClass = "dtc-active";
        }
        else if (data.pending) {
            rawText = data.pending;
            lasterrors = rawText;
            statusClass = "dtc-pending";
        }
        else if (data.cleared) {
            if (lasterrors) {
                rawText = lasterrors;
                lasterrors = "";
            }
            else
                rawText = data.cleared;

            statusClass = "dtc-cleared"
        }
        else if (data.dtc) {
            rawText = data.dtc;
            statusClass = "text";
        }
        if (rawText == "Timeout EBU" || rawText == "Ошибок не обнаружено!" || rawText == "Ошибки успешно удалены!")
            error_list.innerHTML = rawText;

        
        else {
            let errors = rawText.split(', ');
            let htmlCode = "<ul>";


            for (let i = 0; i < errors.length; i++) {
                let errCode = errors[i];
                let errDesc = database[errCode] || "Описание не найдено!";
       
                if (statusClass == "dtc-cleared") {
                    errDesc = "Ошибка успешно стерта из памяти";
                }

                let statusText = "";
                if (statusClass == "dtc-active") statusText = "(Активная)";
                else if (statusClass == "dtc-pending") statusText = "(Временная)";
                else if (statusClass == "dtc-cleared") statusText = "(Удалена)";

                htmlCode += "<li class='" + statusClass + "'>";
                htmlCode += "<strong>" + errCode + " " + statusText + "</strong>";
                htmlCode += "<span style='color: var(--text-main); font-weight: normal; margin-left: 10px;'> &mdash; " + errDesc + "</span>";
                htmlCode += "</li>";
            }

            htmlCode += "</ul>";
            error_list.innerHTML = htmlCode;
        }
        button.innerHTML = "Поиск кодов неисправностей";
        button.disabled = false;

        delbutton.innerHTML = "Сброс ошибок";
        delbutton.disabled = false;
    }
    else if (data.vin)
        vin.innerHTML = data.vin;

    else {
        document.getElementById('rpm').innerHTML = data.rpm;
        document.getElementById('cool').innerHTML = data.cool;
        document.getElementById('throttle').innerHTML = data.throttle;
        document.getElementById('load').innerHTML = data.load;
        document.getElementById('volt').innerHTML = data.volt.toFixed(1);
        document.getElementById('dist').innerHTML = data.dist;
    }
    clearTimeout(pingTimer);
    pingTimer = setTimeout(forceDisconnect, 20000);
}