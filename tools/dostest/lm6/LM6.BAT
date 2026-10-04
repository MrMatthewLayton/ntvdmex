@echo off
if exist H6.TXT del H6.TXT
HELLO.COM > H6.TXT
echo --- H6.TXT, redirected by cmd:
type H6.TXT
