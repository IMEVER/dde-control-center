import QtQuick 2.1
import Deepin.Widgets 1.0

BaseEditLine {
    id: root
    rightLoader.sourceComponent: DSwitchButtonHeader{
        Binding on active {
            when: root.value != undefined
            value: root.value
        }
        // TODO
        // active: {
        //     if (root.value) {
        //         return root.value
        //     } else {
        //         return false
        //     }
        // }
        onActiveChanged: {
            root.value = active
            setKey()
        }
    }
}
