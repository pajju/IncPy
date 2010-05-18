# make an IncPy distribution as IncPy-distro.tar.gz

rm -rf /tmp/IncPy/
rm -f ~/IncPy-distro.tar.gz

cp -aR ~/IncPy/ /tmp
rm -rf /tmp/IncPy/.git
rm -f /tmp/IncPy/.gdb_history
rm -f /tmp/IncPy/*.log
rm -rf /tmp/IncPy/incpy-cache

# .pyc files have hard-coded paths from my own computer ... no good!
find /tmp/IncPy -name '*.pyc' | xargs rm

cd /tmp && tar -cvf ~/IncPy-distro.tar IncPy/
gzip ~/IncPy-distro.tar
echo "Done creating ~/IncPy-distro.tar.gz"

