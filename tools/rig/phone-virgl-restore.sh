#!/system/bin/sh
# Put the APK's libvirglrenderer.so (kept as .orig when a test build was swapped in) back, by rename (a running crosvm keeps
# its open copy), and print both md5s.
L=/data/data/cn.classfun.droidvm/usr/lib/libvirglrenderer.so
[ -f "$L.orig" ] || { echo "no $L.orig"; exit 1; }
echo "before: $(md5sum "$L" | cut -c1-32)  orig: $(md5sum "$L.orig" | cut -c1-32)"
cp -p "$L.orig" "$L.restore" && chown --reference="$L" "$L.restore" 2>/dev/null; chmod --reference="$L" "$L.restore" 2>/dev/null
mv -f "$L.restore" "$L" && echo "after:  $(md5sum "$L" | cut -c1-32)"
ls -l "$L" "$L.orig"
