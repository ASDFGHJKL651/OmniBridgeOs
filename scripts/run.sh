cd d:/omnibridgeos/
make clean
make all
make -C tests/host clean
make -C tests/host all
scripts/merge_sources.py
make -C usr/lib/oblibc
scripts/obcc.sh usr/examples/pipe_test.c -o build/esp/bin/pipe_test.obr --minpriv 0
# 逐一编译测试程序
scripts/obcc.sh usr/examples/syscall_test.c    -o build/esp/bin/syscall_test.obr    --minpriv 0
scripts/obcc.sh usr/examples/signal_test.c     -o build/esp/bin/signal_test.obr     --minpriv 0
scripts/obcc.sh usr/examples/fs_test.c         -o build/esp/bin/fs_test.obr         --minpriv 0
scripts/obcc.sh usr/examples/tls_test.c        -o build/esp/bin/tls_test.obr        --minpriv 0
scripts/obcc.sh usr/examples/exception_test.c  -o build/esp/bin/exception_test.obr  --minpriv 0
make -C tests/host run
scripts/run-qemu.sh