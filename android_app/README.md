# The demo's prebuilt server

`scrcpy-server` is the server of this branch (`server/`, scrcpy 4.1 with the demo's
additions) built with `server/build_without_gradle.sh`. The Zephyr sample
`samples/subsys/mpipe/android_link` pushes and launches it with its
`scripts/launch_android_app.sh`; copy the jar next to that script, or point `SERVER_JAR` at
it. Rebuild it with `BUILD=true server/scripts/build_launch_server.sh` after a change to the
server and copy the result here.
