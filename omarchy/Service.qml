import QtQuick
import Quickshell

// The Omarchy shell side of hyprns. hyprns itself is a Hyprland plugin (hyprns.so),
// not QML: this service only starts load.sh, which builds it (in ~/.cache/hyprns) when
// needed and loads it into Hyprland, and unloads it when the shell plugin is disabled
// or removed.
Item {
  id: root

  // Injected by omarchy-shell.
  property var shell: null
  property var manifest: null

  readonly property string pluginId: "io.github.cristianmoroaica.hyprns"
  readonly property string dir: {
    var url = String(Qt.resolvedUrl("."))
    return decodeURIComponent(url.replace(/^file:\/\//, "")).replace(/\/$/, "")
  }
  readonly property string home: Quickshell.env("HOME")
  readonly property string cacheDir: (Quickshell.env("XDG_CACHE_HOME") || home + "/.cache") + "/hyprns"
  readonly property string shellConfig: (Quickshell.env("XDG_CONFIG_HOME") || home + "/.config") + "/omarchy/shell.json"

  // Detached: the shell reloads every plugin (and so this service) on many occasions,
  // and that must not kill a build halfway. load.sh takes a lock, so a second start
  // waits for the first and then finds hyprns loaded.
  Component.onCompleted: Quickshell.execDetached(["bash", root.dir + "/load.sh"])

  // Destroyed also on every shell restart or plugin reload, when the wallpaper must
  // stay. So the unload waits a moment, then reads the state on disk: it unloads only
  // when the plugin folder is gone, or shell.json is readable and no longer lists the
  // plugin as enabled. Inline, because `omarchy plugin remove` deletes this folder.
  Component.onDestruction: {
    Quickshell.execDetached(["bash", "-c",
      "sleep 3; " +
      "if [ -f \"$2/manifest.json\" ]; then " +
      "  jq -e . \"$3\" >/dev/null 2>&1 || exit 0; " +
      "  jq -e --arg id \"$1\" 'any(.plugins[]?; .id == $id)' \"$3\" >/dev/null && exit 0; " +
      "fi; " +
      "hyprctl plugin unload \"$4\" >/dev/null 2>&1",
      "hyprns-unload", root.pluginId, root.dir.replace(/\/omarchy$/, ""), root.shellConfig,
      root.cacheDir + "/hyprns.so"])
  }
}
