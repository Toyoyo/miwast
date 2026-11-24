# rbtrace.awk - the ST's path trace (vm.c path_line), with what rollbacks
# and loads went back over taken out.
#
#   awk -f tools/rbtrace.awk st.trace > effective.trace
#
# A rollback at a checkpoint goes back to the one before it, which is run
# again and traced again; a load goes back to the checkpoint the save was
# made at. Taking those lines out leaves the path the game ended up on,
# which is a straight path: it must be what the compiler's walk of the
# same menu answers prints (MWC_TRACE), variables and all. The answers are
# the picks left in it, printed last as "#answers a,b,...", and then
# "#refused n", the rolls asked for that the ST could not do.
#
# A rollforward must answer a menu as it was answered before it was rolled
# back over; that is checked here, against the pick the line had then.

function is_cp(line) {
    if (line ~ /^path: (say|pause) /)
        return 1
    return line ~ /^path: menu / && index(line, " [] | bg ") == 0
}

# The last checkpoint line at or before line i, or 0.
function last_cp(i) {
    for (; i > 0; i--)
        if (is_cp(L[i]))
            return i
    return 0
}

function bad(msg) {
    print "rbtrace: " msg > "/dev/stderr"
    status = 1
}

/^path: (say|menu|pause|end|cut)/ {
    L[++n] = $0
    P[n] = ""
    fwd = 0
    next
}

/^path: fwd/ {
    fwd = 1
    next
}

/^path: pick / {
    P[n] = $3
    if (fwd && (OLD[n] == "" || OLDL[n] != L[n]))
        bad("a rollforward at line " n " picked " $3 \
            ", where nothing had been rolled back over")
    else if (fwd && OLD[n] != $3)
        bad("a rollforward at line " n " picked " $3 ", before " OLD[n])
    fwd = 0
    next
}

/^path: back/ {
    x = last_cp(n)
    w = last_cp(x - 1)
    if (!w) {
        bad("a rollback with nothing before line " n)
        next
    }
    # What each menu rolled over was answered, for a rollforward to give
    # again. A menu run again and rolled over before it is answered keeps
    # the answer it had, as its entry in the ST's log does.
    for (i = w; i <= n; i++)
        if (P[i] != "") {
            OLD[i] = P[i]
            OLDL[i] = L[i]
        }
    n = w - 1
    next
}

/^path: refused / {
    refused++
    next
}

/^path: save / {
    s = $3
    SN[s] = n
    for (i = 1; i <= n; i++) {
        SL[s, i] = L[i]
        SP[s, i] = P[i]
    }
    next
}

/^path: load / {
    s = $3
    if (!(s in SN)) {
        bad("slot " s " loaded, never saved")
        next
    }
    n = SN[s]
    for (i = 1; i <= n; i++) {
        L[i] = SL[s, i]
        P[i] = SP[s, i]
    }
    n = last_cp(n) - 1
    next
}

END {
    a = ""
    for (i = 1; i <= n; i++) {
        print L[i]
        if (L[i] ~ /^path: menu / && is_cp(L[i]) && P[i] != "")
            a = a (a == "" ? "" : ",") P[i]
    }
    print "#answers " a
    print "#refused " refused + 0
    exit status
}
