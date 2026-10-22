/* UserStartup.rexx - DataFlyerSCSI's lines in S:User-Startup, and the
   original DataFlyer software's lines out of the startup files.
   Part of DataFlyerSCSI. GPL-3.0-only, see LICENSE.

     RX UserStartup.rexx ADD <units>   write (or replace) our block at the end of
                                       S:User-Startup, and turn the old software's
                                       lines (SCSIPlus, DFMount, XDSFix) in
                                       S:Startup-Sequence and S:User-Startup into
                                       comments
     RX UserStartup.rexx REMOVE        take our block out
     RX UserStartup.rexx RESTORE       turn the old software's lines back on

   <units> is what RDBMount gets: "ALL" or "UNITS=3,4". The first change to a
   file saves a copy as <file>.dfscsi-bak. Returns 0, or 10 when a file could
   not be written. */

parse arg mode units
mode = upper(strip(mode))
units = strip(units)
if units = '' then units = 'ALL'
begin = ';BEGIN DataFlyerSCSI (written by its installer; "Remove" takes it out)'
end   = ';END DataFlyerSCSI'
oldmark = ';DataFlyerSCSI-old '
rc = 0

select
    when mode = 'ADD' then do
        call editfile 'S:User-Startup', 'ADD'
        call editfile 'S:Startup-Sequence', 'OLDOFF'
    end
    when mode = 'REMOVE' then call editfile 'S:User-Startup', 'REMOVE'
    when mode = 'RESTORE' then do
        call editfile 'S:User-Startup', 'OLDON'
        call editfile 'S:Startup-Sequence', 'OLDON'
    end
    otherwise do
        say 'UserStartup.rexx ADD <units> | REMOVE | RESTORE'
        exit 5
    end
end
exit rc

/* one file: read it, rewrite it to T:, copy it back when something changed */
editfile: procedure expose begin end oldmark units rc
    parse arg file, what
    if ~exists(file) then do
        if what ~= 'ADD' then return
        lines.0 = 0
    end
    else do
        if ~open(in, file, 'R') then do; rc = 10; return; end
        n = 0
        do while ~eof(in)
            l = readln(in)
            if eof(in) & l = '' then leave
            n = n + 1
            lines.n = l
        end
        call close in
        lines.0 = n
    end

    changed = 0
    out.0 = 0
    inblock = 0
    do i = 1 to lines.0
        l = lines.i
        /* our own block: dropped (ADD writes a fresh one, REMOVE wants it gone) */
        if (what = 'ADD' | what = 'REMOVE') then do
            if l = begin then do; inblock = 1; changed = 1; iterate; end
            if inblock then do
                if l = end then inblock = 0
                iterate
            end
        end
        /* the old software's lines */
        if what = 'ADD' | what = 'OLDOFF' then do
            if isold(l) then do
                l = oldmark || l
                changed = 1
            end
        end
        if what = 'OLDON' then do
            if left(l, length(oldmark)) = oldmark then do
                l = substr(l, length(oldmark) + 1)
                changed = 1
            end
        end
        call addline l
    end
    /* a trailing blank line before our block */
    if what = 'ADD' then do
        if out.0 > 0 then do
            j = out.0
            if strip(out.j) ~= '' then call addline ''
        end
        call addline begin
        call addline 'C:RDBMount >NIL:' units
        call addline end
        changed = 1
    end
    if ~changed then return

    if ~exists(file || '.dfscsi-bak') then
        address command 'Copy >NIL:' file file || '.dfscsi-bak' 'CLONE'
    tmp = 'T:dfscsi-startup'
    if ~open(o, tmp, 'W') then do; rc = 10; return; end
    do i = 1 to out.0
        call writeln o, out.i
    end
    call close o
    address command 'Copy >NIL:' tmp file
    if RC ~= 0 then rc = 10
    address command 'Delete >NIL:' tmp 'QUIET'
    return

addline: procedure expose out.
    parse arg l
    k = out.0 + 1
    out.k = l
    out.0 = k
    return

/* a line that runs the original DataFlyer software (not already a comment) */
isold: procedure
    parse arg l
    s = strip(l)
    if s = '' then return 0
    if left(s, 1) = ';' then return 0
    u = upper(s)
    if pos('SCSIPLUS', u) > 0 then return 1
    if pos('DFMOUNT', u) > 0 then return 1
    if pos('XDSFIX', u) > 0 then return 1
    return 0
