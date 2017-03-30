/*
 *  ReactOS Task Manager
 *
 *  endproc.c
 *
 *  Copyright (C) 1999 - 2001  Brian Palmer  <brianp@reactos.org>
 *  Copyright (C) 2008  Vladimir Pankratov
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdio.h>
#include <stdlib.h>

#include <windows.h>
#include <commctrl.h>
#include <winnt.h>

#include "wine/unicode.h"
#include "taskmgr.h"
#include "perfdata.h"

static WCHAR wszWarnMsg[511];
static WCHAR wszWarnTitle[255];
static WCHAR wszUnable2Terminate[255];

typedef struct process_list {
    int         *pid;       /*dynamic array to store the process IDs*/
    SIZE_T      count;      /*index to maintain the last entry of the array;*/
    SIZE_T      size;       /*the current size of the pid array*/
} process_list;

static void load_message_strings(void)
{
    LoadStringW(hInst, IDS_TERMINATE_MESSAGE, wszWarnMsg, sizeof(wszWarnMsg)/sizeof(WCHAR));
    LoadStringW(hInst, IDS_TERMINATE_UNABLE2TERMINATE, wszUnable2Terminate, sizeof(wszUnable2Terminate)/sizeof(WCHAR));
    LoadStringW(hInst, IDS_WARNING_TITLE, wszWarnTitle, sizeof(wszWarnTitle)/sizeof(WCHAR));
}

void ProcessPage_OnEndProcess(void)
{
    LVITEMW          lvitem;
    ULONG            Index, Count;
    DWORD            dwProcessId;
    HANDLE           hProcess;
    WCHAR            wstrErrorText[256];

    load_message_strings();

    Count = SendMessageW(hProcessPageListCtrl, LVM_GETITEMCOUNT, 0, 0);
    for (Index=0; Index<Count; Index++)
    {
        lvitem.mask = LVIF_STATE;
        lvitem.stateMask = LVIS_SELECTED;
        lvitem.iItem = Index;
        lvitem.iSubItem = 0;

        SendMessageW(hProcessPageListCtrl, LVM_GETITEMW, 0, (LPARAM) &lvitem);

        if (lvitem.state & LVIS_SELECTED)
            break;
    }

    Count = SendMessageW(hProcessPageListCtrl, LVM_GETSELECTEDCOUNT, 0, 0);
    dwProcessId = PerfDataGetProcessId(Index);
    if ((Count != 1) || (dwProcessId == 0))
        return;

    if (MessageBoxW(hMainWnd, wszWarnMsg, wszWarnTitle, MB_YESNO|MB_ICONWARNING) != IDYES)
        return;

    hProcess = OpenProcess(PROCESS_TERMINATE, FALSE, dwProcessId);

    if (!hProcess)
    {
        GetLastErrorText(wstrErrorText, sizeof(wstrErrorText)/sizeof(WCHAR));
        MessageBoxW(hMainWnd, wstrErrorText,wszUnable2Terminate, MB_OK|MB_ICONSTOP);
        return;
    }

    if (!TerminateProcess(hProcess, 0))
    {
        GetLastErrorText(wstrErrorText, sizeof(wstrErrorText)/sizeof(WCHAR));
        MessageBoxW(hMainWnd, wstrErrorText,wszUnable2Terminate, MB_OK|MB_ICONSTOP);
    }

    CloseHandle(hProcess);
}


static void init_process_list(process_list *list) {
    list->size = 4;            /*initialise size with 4. Will increase if necessary.*/
    list->pid = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size * sizeof(int));
    list->count = 0;
}

static void increase_list_size(process_list *list) {
    list->size *= 2;
    list->pid = HeapRealloc(GetProcessHeap(), HEAP_ZERO_MEMORY, list->pid, list->size * sizeof(int));
}

static void process_list_append(process_list *list, int id) {
    if(list->count == list->size)
        increase_list_size(list);
    list->pid[list->count] = id;
    list->count += 1;
}

static void free_process_list(process_list *list) {
    HeapFree(GetProcessHeap(), 0, list->pid)
}


static void enum_process_children(HANDLE snapshot, process_list *list, DWORD pid) {
    PROCESSENTRY32 entry, 
    SIZE_T start, end, i;

    start = list->count;

    entry.dwSize = sizeof(entry);

    if(!Process32First(snapshot, &entry))
        return;

    do 
    {
        if(entry.th32ParentProcessID == pid)
            process_list_append(list, entry.th32ProcessID);
    } while (Process32Next(snapshot, &entry));

    end = list->count;

    for(i = start; i < end; ++i)
    {
        enum_process_children(snapshot, list, list->pid[i]);
    }
}


void ProcessPage_OnEndProcessTree(void)
{
    LVITEMW          lvitem;
    ULONG            Index, Count;
    DWORD            dwProcessId;
    HANDLE           hProcess;
    WCHAR            wstrErrorText[256];
    process_list     list;
    SIZE_T           i;
    HANDLE           snapshot;

    load_message_strings();

    Count = SendMessageW(hProcessPageListCtrl, LVM_GETITEMCOUNT, 0, 0);
    for (Index=0; Index<Count; Index++)
    {
        lvitem.mask = LVIF_STATE;
        lvitem.stateMask = LVIS_SELECTED;
        lvitem.iItem = Index;
        lvitem.iSubItem = 0;

        SendMessageW(hProcessPageListCtrl, LVM_GETITEMW, 0, (LPARAM) &lvitem);

        if (lvitem.state & LVIS_SELECTED)
            break;
    }

    Count = SendMessageW(hProcessPageListCtrl, LVM_GETSELECTEDCOUNT, 0, 0);
    dwProcessId = PerfDataGetProcessId(Index);
    if ((Count != 1) || (dwProcessId == 0))
        return;

    if (MessageBoxW(hMainWnd, wszWarnMsg, wszWarnTitle, MB_YESNO|MB_ICONWARNING) != IDYES)
        return;


    init_process_list(&list);

    if(list->pid == NULL)
        return;

    process_list_append(&list, dwProcessId);

    snapshot = CreateToolhelp32Snapshot(TH32SNAPPROCESS, 0);

    if(!snapshot)
        return;

    enum_process_children(snapshot, &list, dwProcessId);
    
    CloseHandle(snapshot);

    for(i = 0; i < list->count; ++i) {
        hProcess = OpenProcess(PROCESS_TERMINATE, FALSE, list->pid[i]);        

        if (!hProcess)
        {
            GetLastErrorText(wstrErrorText, sizeof(wstrErrorText)/sizeof(WCHAR));
            MessageBoxW(hMainWnd, wstrErrorText,wszUnable2Terminate, MB_OK|MB_ICONSTOP);
            break;
        }

        if (!TerminateProcess(hProcess, 0))
        {
            GetLastErrorText(wstrErrorText, sizeof(wstrErrorText)/sizeof(WCHAR));
            MessageBoxW(hMainWnd, wstrErrorText,wszUnable2Terminate, MB_OK|MB_ICONSTOP);
        }
        CloseHandle(hProcess);
    }

    free_process_list(list);
}