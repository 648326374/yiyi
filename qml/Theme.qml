pragma Singleton
import QtQuick

QtObject {
    readonly property string preference: themeOverride.length ? themeOverride : App.theme
    readonly property bool dark: preference === "dark" || (preference === "system" && Qt.styleHints.colorScheme === Qt.Dark)
    readonly property color background: dark ? "#131b18" : "#f6f8f6"
    readonly property color sidebar: dark ? "#17221d" : "#eef3ef"
    readonly property color surface: dark ? "#1c2721" : "#ffffff"
    readonly property color subtle: dark ? "#233129" : "#f5f8f5"
    readonly property color hover: dark ? "#2a3a30" : "#e8eeea"
    readonly property color text: dark ? "#e7eee9" : "#263d32"
    readonly property color secondary: dark ? "#acbcb1" : "#758379"
    readonly property color muted: dark ? "#7d9385" : "#99a69c"
    readonly property color border: dark ? "#324037" : "#e4ebe5"
    readonly property color accent: dark ? "#86c5a2" : "#347457"
    readonly property color accentStrong: dark ? "#a1d6b6" : "#285e46"
    readonly property color accentSoft: dark ? "#2a4032" : "#eaf3ec"
    readonly property color accentSoftHover: dark ? "#344e3d" : "#deece2"
    readonly property color accentSoftPressed: dark ? "#3b5845" : "#d2e4d8"
    readonly property color accentText: dark ? "#14271c" : "#ffffff"
    readonly property color danger: dark ? "#efab9f" : "#af5349"
    readonly property color dangerSoft: dark ? "#3b2a27" : "#fff1ec"
    readonly property color warning: dark ? "#e5b86c" : "#986315"
    readonly property string font: "Microsoft YaHei UI"
}
