#!/bin/sh
# Runs the user-space tests against the three builds of ziprTool:
#   B/ziprTool       release build, stored only
#   B/san/ziprTool   with ASan and UBSan, stored only
#   B/zlib/ziprTool  with sanitizers and deflate via system zlib
#
#   tests/run.sh BUILD_DIR
set -eu

B=${1:-build}
T=$B/tests
PLAIN=$B/ziprTool
SAN=$B/san/ziprTool
ZLIB=$B/zlib/ziprTool

# Sanitizer reports must not look like an ordinary error (status 1).
export ASAN_OPTIONS=exitcode=99
export UBSAN_OPTIONS=halt_on_error=1:exitcode=98:print_stacktrace=1

rm -rf "$T"
mkdir -p "$T"
python3 tests/mkZips.py "$T"

pass=0
fail=0
ok()  { pass=$((pass + 1)); }
bad() { fail=$((fail + 1)); echo "FAIL: $*"; }

# check DESCRIPTION COMMAND...: the command must succeed.
check() {
	desc=$1
	shift
	if "$@" >"$T/out" 2>&1; then ok; else bad "$desc"; cat "$T/out"; fi
}

# check_err DESCRIPTION MESSAGE COMMAND...: exit status 1 with MESSAGE.
check_err() {
	desc=$1
	msg=$2
	shift 2
	set +e
	"$@" >"$T/out" 2>&1
	st=$?
	set -e
	if [ "$st" -eq 1 ] && grep -q "$msg" "$T/out"; then
		ok
	else
		bad "$desc (status $st)"
		cat "$T/out"
	fi
}

# extract TOOL ARCHIVE: every member matches its reference at several
# buffer sizes; 1-byte reads only for the small files.
extract() {
	for name in hello.txt empty.txt data/random.bin data/text.txt; do
		for bs in 7 4096 65536; do
			check "$1 x $2 $name -b $bs" \
				sh -c "'$1' -b $bs x '$T/$2' '$name' | cmp - '$T/ref/$name'"
		done
	done
	check "$1 x $2 hello.txt -b 1" \
		sh -c "'$1' -b 1 x '$T/$2' hello.txt | cmp - '$T/ref/hello.txt'"
}

for tool in "$PLAIN" "$SAN" "$ZLIB"; do
	check "$tool l stored.zip" sh -c "'$tool' l '$T/stored.zip' | grep -q '^5 entries'"
	check "$tool t stored.zip" "$tool" t "$T/stored.zip"
	extract "$tool" stored.zip
	extract "$tool" comment.zip
	extract "$tool" prefixed.zip
	check "$tool x zip64extra.zip" \
		sh -c "'$tool' x '$T/zip64extra.zip' big.txt | cmp - '$T/ref/big.txt'"
	check "$tool l many.zip" sh -c "'$tool' l '$T/many.zip' | grep -q '^70000 entries'"
	check "$tool t many.zip" "$tool" t "$T/many.zip"
	check_err "$tool t badcrc.zip" "FAIL hello.txt: Bad message" "$tool" t "$T/badcrc.zip"
	check_err "$tool x missing" "No such file" "$tool" x "$T/stored.zip" nope.txt
	check_err "$tool truncated.zip" "Invalid argument" "$tool" l "$T/truncated.zip"
	check_err "$tool notzip.bin" "Invalid argument" "$tool" l "$T/notzip.bin"
	check_err "$tool directory" "Is a directory" "$tool" l "$T"
	check "$tool l wrapped.zip" sh -c "'$tool' l '$T/wrapped.zip' | grep -q '^70000 entries'"
	check "$tool l bigentries.zip" sh -c "'$tool' l '$T/bigentries.zip' | grep -q '^10 entries'"
	check "$tool t bigentries.zip" "$tool" t "$T/bigentries.zip"
	for idx in "" -i; do
		check "$tool $idx x wrapped.zip" \
			sh -c "test \"\$('$tool' $idx x '$T/wrapped.zip' w69999)\" = 69999"
		check "$tool $idx f stored.zip" "$tool" $idx f "$T/stored.zip" \
			hello.txt empty.txt data/ data/random.bin data/text.txt
		check_err "$tool $idx f missing" "No such file" \
			"$tool" $idx f "$T/stored.zip" hello.txt nope.txt
		check "$tool $idx f many.zip" "$tool" $idx f "$T/many.zip" f00000 f34567 f69999
		check "$tool $idx f bigentries.zip" \
			sh -c "xargs -d '\n' '$tool' $idx f '$T/bigentries.zip' <'$T/bignames.txt'"
		check "$tool $idx x prefixed.zip" \
			sh -c "'$tool' $idx x '$T/prefixed.zip' data/text.txt | cmp - '$T/ref/data/text.txt'"
		check "$tool $idx x zip64extra.zip" \
			sh -c "'$tool' $idx x '$T/zip64extra.zip' big.txt | cmp - '$T/ref/big.txt'"
	done
done

# Deflate: unsupported in user space, supported by the kernel's code path.
for tool in "$PLAIN" "$SAN"; do
	check_err "$tool t deflated.zip" "Operation not supported" "$tool" t "$T/deflated.zip"
done
check "$ZLIB t deflated.zip" "$ZLIB" t "$T/deflated.zip"
extract "$ZLIB" deflated.zip
check_err "$ZLIB t baddeflate.zip" "FAIL data/text.txt: Bad message" \
	"$ZLIB" t "$T/baddeflate.zip"

# Fuzz: damaged archives may fail, but must never trip a sanitizer.
for f in "$T"/fuzz/*.zip; do
	for tool in "$SAN" "$ZLIB"; do
		set +e
		for args in "-b 333 t $f" "-i f $f hello.txt data/text.txt"; do
			# shellcheck disable=SC2086
			"$tool" $args >"$T/out" 2>&1
			st=$?
			if [ "$st" -le 1 ]; then ok; else bad "$tool $args (status $st)"; cat "$T/out"; fi
		done
		set -e
	done
done

echo "passed: $pass, failed: $fail"
[ "$fail" -eq 0 ]
