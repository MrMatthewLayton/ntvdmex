; t3f.com -- #251: read stdin (handle 0) twice and record what came back.
        org 100h
        mov ah,3Fh
        xor bx,bx
        mov cx,2
        mov dx,b1
        int 21h
        mov [n1],ax
        mov ah,3Fh
        xor bx,bx
        mov cx,10
        mov dx,b2
        int 21h
        mov [n2],ax
        mov ah,3Ch              ; create the report
        xor cx,cx
        mov dx,fname
        int 21h
        mov bx,ax
        mov ah,40h
        mov cx,end-n1
        mov dx,n1
        int 21h
        mov ah,3Eh
        int 21h
        mov ax,4C00h
        int 21h
fname db 'C:\DOCUME~1\ALLUSE~1\DOCUME~1\NTVDMEX\DEBUG\OUT\T3F.BIN',0
n1 dw 0FFFFh
n2 dw 0FFFFh
b1 db 0EEh,0EEh
b2 db 0EEh,0EEh,0EEh,0EEh,0EEh,0EEh,0EEh,0EEh,0EEh,0EEh
end:
