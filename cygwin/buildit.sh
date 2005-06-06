#!/bin/bash -ex

TRGDIR=/cygdrive/c/csync2

if ! [ -f sqlite-2.8.16.tar.gz ]; then
	wget http://www.sqlite.org/sqlite-2.8.16.tar.gz -O sqlite-2.8.16.tar.gz
fi

if ! [ -f librsync-0.9.7.tar.gz ]; then
	wget http://mesh.dl.sourceforge.net/sourceforge/librsync/librsync-0.9.7.tar.gz -O librsync-0.9.7.tar.gz
fi

if ! [ -f SQLite.NET.2.0.1.zip ]; then
	wget http://www.phpguru.org/downloads/csharp/SQLite.NET/SQLite.NET.2.0.1.zip -O SQLite.NET.2.0.1.zip
fi

if ! [ -f SQLiteClient.dll ]; then
	unzip -p SQLite.NET.2.0.1.zip SQLite.NET/SQLiteClient/bin/Release/SQLiteClient.dll > SQLiteClient.dll
fi

if ! [ -f sqlite.dll ]; then
	unzip -p SQLite.NET.2.0.1.zip SQLite.NET/SQLiteTest/bin/Release/sqlite.dll > sqlite.dll
fi

cd ..
mkdir -p $TRGDIR

if ! [ -f config.h ]; then
	./configure \
		--with-librsync-source=cygwin/librsync-0.9.7.tar.gz \
		--with-libsqlite-source=cygwin/sqlite-2.8.16.tar.gz \
		--sysconfdir=$TRGDIR
fi

make private_librsync
make private_libsqlite
make CFLAGS='-DREAL_DBDIR=\".\"'

ignore_dlls="KERNEL32.dll"
copy_dlls() {
	for dll in $( strings $1 | egrep '^[^ ]+\.dll$' | sort -u; )
	do
		if echo "$dll" | egrep -qv "^($ignore_dlls)\$"
		then
			cp -v /bin/$dll $TRGDIR/$dll
			ignore_dlls="$ignore_dlls|$dll"
			copy_dlls $TRGDIR/$dll
		fi
	done
}

cp -v csync2.exe $TRGDIR/csync2.exe
copy_dlls $TRGDIR/csync2.exe

cd cygwin
PATH="$PATH:/cygdrive/c/WINNT/Microsoft.NET/Framework/v1.0.3705"
csc /nologo /r:SQLiteClient.dll cs2_hintd_win32.cs

gcc -Wall monitor.c -o monitor.exe -DTRGDIR="\"$TRGDIR"\"

cp -v readme_pkg.txt $TRGDIR/README.txt
cp -v SQLiteClient.dll sqlite.dll $TRGDIR/
cp -v cs2_hintd_win32.exe monitor.exe $TRGDIR/

cd $( dirname $TRGDIR/ )
rm -f $( basename $TRGDIR ).zip
zip -r $( basename $TRGDIR ).zip $( basename $TRGDIR ) \
	-i '*.txt' '*.dll' '*.exe'

echo "DONE."
