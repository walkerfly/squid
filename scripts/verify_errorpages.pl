#!/usr/bin/perl

open(LIST,"<list") || die;
while (<LIST>) {
    chomp;
    push (@pages, $_);
}
close LIST;

foreach $page (@pages) {
    open(IN, "<English/$page") || die;
    $file = join("", <IN>);
    close(IN);
    $file =~ s/%(.)/$english{$page}{$1}++, "%$1"/ge;
}

foreach $lang (@ARGV) {
  foreach $page (@pages) {
    undef %codes;
    open(IN, "<$lang/$page") || die;
    $file = join("", <IN>);
    close(IN);
    $file =~ s/%(.)/$codes{$1}++/ge;
    foreach $code (keys %codes, keys %{$english{$page}}) {
	if ($codes{$code} ne $english{$page}{$code}) {
	    print("$lang/$page %$code mismatch (found $codes{$code}, expected $english{$page}{$code})\n");
	}
    }
  }
}
