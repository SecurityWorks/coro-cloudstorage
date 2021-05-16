"use strict";

function sizeToString(d) {
    if (d < 1000) {
        return d + " B";
    } else if (d < 1000) {
        return (d / 1000).toFixed(2) + " KB";
    } else if (d < 1000000) {
        return (d / 1000000).toFixed(2) + " MB";
    } else {
        return (d / 1000000000).toFixed(2) + " GB";
    }
}

function initialize() {
    document.querySelectorAll("[id^=size-]").forEach((e) => {
        let account_id = e.id.substr("size-".length);
        fetch("/size?account_id=" + account_id).then(response => response.json()).then(json => {
            if (json && json.hasOwnProperty("space_used")) {
                let size = "";
                size += sizeToString(json.space_used);
                if (json.hasOwnProperty("space_total")) {
                    size += " / " + sizeToString(json.space_total);
                }
                e.innerHTML = size;
            }
        });
    });
}

window.onload = (event) => {
    initialize();
};
