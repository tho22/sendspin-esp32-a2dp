// Embedded into the web interface (web_server js_include).
// The web interface reads the options of a select only when the page loads, so the speakers found by
// "Search speakers" would only show up after a manual reload. Reload the page once the search has finished.
const SEARCHING = "Searching for speakers ready to pair";

function watch(source) {
  let searching = false;
  source.addEventListener("state", (event) => {
    const data = JSON.parse(event.data);
    const id = data.name_id || data.id || "";
    if (!id.endsWith("Bluetooth status")) {
      return;
    }
    if (data.value === SEARCHING) {
      searching = true;
    } else if (searching) {
      location.reload();
    }
  });
}

// window.source (the event stream) is created by the web interface script
const timer = setInterval(() => {
  if (window.source) {
    clearInterval(timer);
    watch(window.source);
  }
}, 100);
