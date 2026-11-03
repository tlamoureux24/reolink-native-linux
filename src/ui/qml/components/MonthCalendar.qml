import QtQuick
import QtQuick.Layouts
import ReolinkApp
import ReolinkApp.Core

// Compact month calendar. Emits dateSelected(year, month, day) — month is
// 1-based. Days that have recordings can be marked via markedDays.
// Stateless: year/month (the month shown) and selYear/selMonth/selDay (the
// selected date) are owned by the parent. The arrows emit monthNavigated and
// a tap emits dateSelected; assigning these properties here would break the
// parent's bindings and leave the calendar stuck on a stale month/selection.
Column {
    id: root
    spacing: 6

    property int year: 2026
    property int month: 7           // 1-based
    property int selYear: year
    property int selMonth: month
    property int selDay: 9
    property var markedDays: []      // days (1..31) of the SHOWN month with recordings

    signal dateSelected(int year, int month, int day)
    signal monthNavigated(int year, int month)

    function daysInMonth(y, m) { return new Date(y, m, 0).getDate(); }
    // Weeks start on the locale's first day (Monday in French, Sunday in US
    // English); headers use the locale's own narrow day names.
    readonly property var loc: Qt.locale(AppSettings.localeName) // the app's language
    readonly property int weekStart: loc.firstDayOfWeek % 7 // Sunday = 0
    function firstWeekday(y, m) { return (new Date(y, m - 1, 1).getDay() - weekStart + 7) % 7; }

    RowLayout {
        width: parent.width
        Rectangle {
            width: 24; height: 24; radius: 4
            color: prevHover.hovered ? Theme.surfaceAlt : "transparent"
            Text { anchors.centerIn: parent; text: "‹"; color: Theme.text; font.pixelSize: 16 }
            HoverHandler { id: prevHover }
            TapHandler { onTapped: {
                if (root.month === 1) root.monthNavigated(root.year - 1, 12);
                else root.monthNavigated(root.year, root.month - 1);
            } }
        }
        Text {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            text: root.loc.standaloneMonthName(root.month - 1) + " " + root.year
            color: Theme.text
            font.pixelSize: 13
            font.bold: true
        }
        Rectangle {
            width: 24; height: 24; radius: 4
            color: nextHover.hovered ? Theme.surfaceAlt : "transparent"
            Text { anchors.centerIn: parent; text: "›"; color: Theme.text; font.pixelSize: 16 }
            HoverHandler { id: nextHover }
            TapHandler { onTapped: {
                if (root.month === 12) root.monthNavigated(root.year + 1, 1);
                else root.monthNavigated(root.year, root.month + 1);
            } }
        }
    }

    Grid {
        columns: 7
        spacing: 2
        Repeater {
            model: [0, 1, 2, 3, 4, 5, 6].map(function (i) {
                return root.loc.dayName((root.weekStart + i) % 7, Locale.NarrowFormat);
            })
            Text {
                required property string modelData
                width: 30; height: 18
                horizontalAlignment: Text.AlignHCenter
                text: modelData
                color: Theme.textMuted
                font.pixelSize: 10
            }
        }
    }

    Grid {
        columns: 7
        spacing: 2
        Repeater {
            model: 42
            Item {
                required property int index
                property int dayNum: index - root.firstWeekday(root.year, root.month) + 1
                property bool valid: dayNum >= 1 && dayNum <= root.daysInMonth(root.year, root.month)
                property bool selected: valid && dayNum === root.selDay
                                        && root.year === root.selYear && root.month === root.selMonth
                width: 30; height: 28
                Rectangle {
                    anchors.fill: parent
                    radius: 4
                    visible: parent.valid
                    color: parent.selected ? Theme.accentDim
                         : dayHover.hovered ? Theme.surfaceAlt : "transparent"
                    Text {
                        anchors.centerIn: parent
                        text: parent.parent.valid ? parent.parent.dayNum : ""
                        color: parent.parent.selected ? Theme.text : Theme.textMuted
                        font.pixelSize: 11
                    }
                    Rectangle { // recording marker
                        visible: root.markedDays.indexOf(parent.parent.dayNum) >= 0
                        width: 4; height: 4; radius: 2
                        color: Theme.accent
                        anchors.bottom: parent.bottom
                        anchors.bottomMargin: 2
                        anchors.horizontalCenter: parent.horizontalCenter
                    }
                    HoverHandler { id: dayHover; enabled: parent.parent.valid }
                    TapHandler {
                        enabled: parent.parent.valid
                        onTapped: root.dateSelected(root.year, root.month, parent.parent.dayNum)
                    }
                }
            }
        }
    }
}
