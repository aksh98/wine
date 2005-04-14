/*
 * Compound Storage (32 bit version)
 * Storage implementation
 *
 * This file contains the compound file implementation
 * of the storage interface.
 *
 * Copyright 1999 Francis Beaudet
 * Copyright 1999 Sylvain St-Germain
 * Copyright 1999 Thuy Nguyen
 * Copyright 2005 Mike McCormack
 * Copyright 2005 Juan Lang
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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * There's a decent overview of property set storage here:
 * http://msdn.microsoft.com/archive/en-us/dnarolegen/html/msdn_propset.asp
 * It's a little bit out of date, and more definitive references are given
 * below, but it gives the best "big picture" that I've found.
 *
 * TODO: There's a lot missing in here.  Biggies:
 * - There are all sorts of restricions I don't honor, like maximum property
 *   set byte size, maximum property name length
 * - Certain bogus files could result in reading past the end of a buffer.
 * - This will probably fail on big-endian machines, especially reading and
 *   writing strings.
 * - Mac-generated files won't be read correctly, even if they're little
 *   endian, because I disregard whether the generator was a Mac.  This means
 *   strings will probably be munged (as I don't understand Mac scripts.)
 * - Not all PROPVARIANT types are supported.
 * There are lots more unimplemented features, see FIXMEs below.
 */

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COBJMACROS
#define NONAMELESSUNION
#define NONAMELESSSTRUCT

#include "windef.h"
#include "winbase.h"
#include "winnls.h"
#include "winuser.h"
#include "wine/unicode.h"
#include "wine/debug.h"
#include "dictionary.h"
#include "storage32.h"

WINE_DEFAULT_DEBUG_CHANNEL(storage);

#define _IPropertySetStorage_Offset ((int)(&(((StorageImpl*)0)->base.pssVtbl)))
#define _ICOM_THIS_From_IPropertySetStorage(class, name) \
    class* This = (class*)(((char*)name)-_IPropertySetStorage_Offset)

/* These are documented in MSDN, e.g.
 * http://msdn.microsoft.com/library/en-us/stg/stg/property_set_header.asp
 * http://msdn.microsoft.com/library/library/en-us/stg/stg/section.asp
 * but they don't seem to be in any header file.
 */
#define PROPSETHDR_BYTEORDER_MAGIC      0xfffe
#define PROPSETHDR_OSVER_KIND_WIN16     0
#define PROPSETHDR_OSVER_KIND_MAC       1
#define PROPSETHDR_OSVER_KIND_WIN32     2

#define CP_UNICODE 1200

/* The format version (and what it implies) is described here:
 * http://msdn.microsoft.com/library/en-us/stg/stg/format_version.asp
 */
typedef struct tagPROPERTYSETHEADER
{
    WORD  wByteOrder; /* always 0xfffe */
    WORD  wFormat;    /* can be zero or one */
    DWORD dwOSVer;    /* OS version of originating system */
    CLSID clsid;      /* application CLSID */
    DWORD reserved;   /* always 1 */
} PROPERTYSETHEADER;

typedef struct tagFORMATIDOFFSET
{
    FMTID fmtid;
    DWORD dwOffset; /* from beginning of stream */
} FORMATIDOFFSET;

typedef struct tagPROPERTYSECTIONHEADER
{
    DWORD cbSection;
    DWORD cProperties;
} PROPERTYSECTIONHEADER;

typedef struct tagPROPERTYIDOFFSET
{
    DWORD propid;
    DWORD dwOffset; /* from beginning of section */
} PROPERTYIDOFFSET;

struct tagPropertyStorage_impl;

/* Initializes the property storage from the stream (and undoes any uncommitted
 * changes in the process.)  Returns an error if there is an error reading or
 * if the stream format doesn't match what's expected.
 */
static HRESULT PropertyStorage_ReadFromStream(struct tagPropertyStorage_impl *);

static HRESULT PropertyStorage_WriteToStream(struct tagPropertyStorage_impl *);

/* Creates the dictionaries used by the property storage.  If successful, all
 * the dictionaries have been created.  If failed, none has been.  (This makes
 * it a bit easier to deal with destroying them.)
 */
static HRESULT PropertyStorage_CreateDictionaries(
 struct tagPropertyStorage_impl *);

static void PropertyStorage_DestroyDictionaries(
 struct tagPropertyStorage_impl *);

static IPropertyStorageVtbl IPropertyStorage_Vtbl;

/***********************************************************************
 * Implementation of IPropertyStorage
 */
typedef struct tagPropertyStorage_impl
{
    IPropertyStorageVtbl *vtbl;
    DWORD ref;
    CRITICAL_SECTION cs;
    IStream *stm;
    BOOL  dirty;
    FMTID fmtid;
    CLSID clsid;
    DWORD originatorOS;
    DWORD grfFlags;
    DWORD grfMode;
    UINT  codePage;
    LCID  locale;
    PROPID highestProp;
    struct dictionary *name_to_propid;
    struct dictionary *propid_to_name;
    struct dictionary *propid_to_prop;
} PropertyStorage_impl;

/************************************************************************
 * IPropertyStorage_fnQueryInterface (IPropertyStorage)
 */
static HRESULT WINAPI IPropertyStorage_fnQueryInterface(
    IPropertyStorage *iface,
    REFIID riid,
    void** ppvObject)
{
    PropertyStorage_impl *This = (PropertyStorage_impl *)iface;

    if ( (This==0) || (ppvObject==0) )
        return E_INVALIDARG;

    *ppvObject = 0;

    if (IsEqualGUID(&IID_IUnknown, riid) ||
        IsEqualGUID(&IID_IPropertyStorage, riid))
    {
        IPropertyStorage_AddRef(iface);
        *ppvObject = (IPropertyStorage*)iface;
        return S_OK;
    }

    return E_NOINTERFACE;
}

/************************************************************************
 * IPropertyStorage_fnAddRef (IPropertyStorage)
 */
static ULONG WINAPI IPropertyStorage_fnAddRef(
    IPropertyStorage *iface)
{
    PropertyStorage_impl *This = (PropertyStorage_impl *)iface;
    return InterlockedIncrement(&This->ref);
}

/************************************************************************
 * IPropertyStorage_fnRelease (IPropertyStorage)
 */
static ULONG WINAPI IPropertyStorage_fnRelease(
    IPropertyStorage *iface)
{
    PropertyStorage_impl *This = (PropertyStorage_impl *)iface;
    ULONG ref;

    ref = InterlockedDecrement(&This->ref);
    if (ref == 0)
    {
        TRACE("Destroying %p\n", This);
        if (This->dirty)
            IPropertyStorage_Commit(iface, STGC_DEFAULT);
        IStream_Release(This->stm);
        DeleteCriticalSection(&This->cs);
        PropertyStorage_DestroyDictionaries(This);
        HeapFree(GetProcessHeap(), 0, This);
    }
    return ref;
}

static PROPVARIANT *PropertyStorage_FindProperty(PropertyStorage_impl *This,
 DWORD propid)
{
    PROPVARIANT *ret = NULL;

    assert(This);
    dictionary_find(This->propid_to_prop, (void *)propid, (void **)&ret);
    TRACE("returning %p\n", ret);
    return ret;
}

/* Returns NULL if name is NULL. */
static PROPVARIANT *PropertyStorage_FindPropertyByName(
 PropertyStorage_impl *This, LPCWSTR name)
{
    PROPVARIANT *ret = NULL;
    PROPID propid;

    assert(This);
    if (!name)
        return NULL;
    if (dictionary_find(This->name_to_propid, name, (void **)&propid))
        ret = PropertyStorage_FindProperty(This, (PROPID)propid);
    TRACE("returning %p\n", ret);
    return ret;
}

static LPWSTR PropertyStorage_FindPropertyNameById(PropertyStorage_impl *This,
 DWORD propid)
{
    LPWSTR ret = NULL;

    assert(This);
    dictionary_find(This->propid_to_name, (void *)propid, (void **)&ret);
    TRACE("returning %p\n", ret);
    return ret;
}

/************************************************************************
 * IPropertyStorage_fnReadMultiple (IPropertyStorage)
 */
static HRESULT WINAPI IPropertyStorage_fnReadMultiple(
    IPropertyStorage* iface,
    ULONG cpspec,
    const PROPSPEC rgpspec[],
    PROPVARIANT rgpropvar[])
{
    PropertyStorage_impl *This = (PropertyStorage_impl *)iface;
    HRESULT hr = S_FALSE;
    ULONG i;

    TRACE("(%p, %ld, %p, %p)\n", iface, cpspec, rgpspec, rgpropvar);
    if (!This)
        return E_INVALIDARG;
    if (cpspec && (!rgpspec || !rgpropvar))
        return E_INVALIDARG;
    EnterCriticalSection(&This->cs);
    for (i = 0; i < cpspec; i++)
    {
        PropVariantInit(&rgpropvar[i]);
        if (rgpspec[i].ulKind == PRSPEC_LPWSTR)
        {
            PROPVARIANT *prop = PropertyStorage_FindPropertyByName(This,
             rgpspec[i].u.lpwstr);

            if (prop)
                PropVariantCopy(&rgpropvar[i], prop);
        }
        else
        {
            PROPVARIANT *prop = PropertyStorage_FindProperty(This,
             rgpspec[i].u.propid);

            if (prop)
                PropVariantCopy(&rgpropvar[i], prop);
        }
    }
    LeaveCriticalSection(&This->cs);
    return hr;
}

static HRESULT PropertyStorage_StorePropWithId(PropertyStorage_impl *This,
 PROPID propid, const PROPVARIANT *propvar)
{
    HRESULT hr = S_OK;
    PROPVARIANT *prop = PropertyStorage_FindProperty(This, propid);

    assert(This);
    assert(propvar);
    TRACE("Setting 0x%08lx to type %d\n", propid, propvar->vt);
    if (prop)
    {
        PropVariantClear(prop);
        PropVariantCopy(prop, propvar);
    }
    else
    {
        prop = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
         sizeof(PROPVARIANT));
        if (prop)
        {
            PropVariantCopy(prop, propvar);
            dictionary_insert(This->propid_to_prop, (void *)propid, prop);
            if (propid > This->highestProp)
                This->highestProp = propid;
        }
        else
            hr = STG_E_INSUFFICIENTMEMORY;
    }
    return hr;
}

/************************************************************************
 * IPropertyStorage_fnWriteMultiple (IPropertyStorage)
 */
static HRESULT WINAPI IPropertyStorage_fnWriteMultiple(
    IPropertyStorage* iface,
    ULONG cpspec,
    const PROPSPEC rgpspec[],
    const PROPVARIANT rgpropvar[],
    PROPID propidNameFirst)
{
    PropertyStorage_impl *This = (PropertyStorage_impl *)iface;
    HRESULT hr = S_OK;
    ULONG i;

    TRACE("(%p, %ld, %p, %p)\n", iface, cpspec, rgpspec, rgpropvar);
    if (!This)
        return E_INVALIDARG;
    if (cpspec && (!rgpspec || !rgpropvar))
        return E_INVALIDARG;
    if (!(This->grfMode & STGM_READWRITE))
        return STG_E_ACCESSDENIED;
    EnterCriticalSection(&This->cs);
    This->dirty = TRUE;
    This->originatorOS = (DWORD)MAKELONG(LOWORD(GetVersion()),
     PROPSETHDR_OSVER_KIND_WIN32) ;
    for (i = 0; i < cpspec; i++)
    {
        if (rgpspec[i].ulKind == PRSPEC_LPWSTR)
        {
            PROPVARIANT *prop = PropertyStorage_FindPropertyByName(This,
             rgpspec[i].u.lpwstr);

            if (prop)
                PropVariantCopy(prop, &rgpropvar[i]);
            else
            {
                /* Note that I don't do the special cases here that I do below,
                 * because naming the special PIDs isn't supported.
                 */
                if (propidNameFirst < PID_FIRST_USABLE ||
                 propidNameFirst >= PID_MIN_READONLY)
                    hr = STG_E_INVALIDPARAMETER;
                else
                {
                    PROPID nextId = max(propidNameFirst, This->highestProp + 1);
                    size_t len = strlenW(rgpspec[i].u.lpwstr) + 1;
                    LPWSTR name = HeapAlloc(GetProcessHeap(), 0,
                     len * sizeof(WCHAR));

                    strcpyW(name, rgpspec[i].u.lpwstr);
                    TRACE("Adding prop name %s, propid %ld\n", debugstr_w(name),
                     nextId);
                    dictionary_insert(This->name_to_propid, name,
                     (void *)nextId);
                    dictionary_insert(This->propid_to_name, (void *)nextId,
                     name);
                    hr = PropertyStorage_StorePropWithId(This, nextId,
                     &rgpropvar[i]);
                }
            }
        }
        else
        {
            switch (rgpspec[i].u.propid)
            {
            case PID_DICTIONARY:
                /* Can't set the dictionary */
                hr = STG_E_INVALIDPARAMETER;
                break;
            case PID_CODEPAGE:
                /* Can only set the code page if nothing else has been set */
                if (dictionary_num_entries(This->propid_to_prop) == 0 &&
                 rgpropvar[i].vt == VT_I2)
                    This->codePage = rgpropvar[i].u.iVal;
                else
                    hr = STG_E_INVALIDPARAMETER;
                break;
            case PID_LOCALE:
                /* Can only set the locale if nothing else has been set */
                if (dictionary_num_entries(This->propid_to_prop) == 0 &&
                 rgpropvar[i].vt == VT_I4)
                    This->locale = rgpropvar[i].u.lVal;
                else
                    hr = STG_E_INVALIDPARAMETER;
                break;
            case PID_ILLEGAL:
                /* silently ignore like MSDN says */
                break;
            default:
                if (rgpspec[i].u.propid >= PID_MIN_READONLY)
                    hr = STG_E_INVALIDPARAMETER;
                else
                    hr = PropertyStorage_StorePropWithId(This,
                     rgpspec[i].u.propid, &rgpropvar[i]);
            }
        }
    }
    if (This->grfFlags & PROPSETFLAG_UNBUFFERED)
        IPropertyStorage_Commit(iface, STGC_DEFAULT);
    LeaveCriticalSection(&This->cs);
    return hr;
}

/************************************************************************
 * IPropertyStorage_fnDeleteMultiple (IPropertyStorage)
 */
static HRESULT WINAPI IPropertyStorage_fnDeleteMultiple(
    IPropertyStorage* iface,
    ULONG cpspec,
    const PROPSPEC rgpspec[])
{
    PropertyStorage_impl *This = (PropertyStorage_impl *)iface;
    ULONG i;
    HRESULT hr;

    TRACE("(%p, %ld, %p)\n", iface, cpspec, rgpspec);
    if (!This)
        return E_INVALIDARG;
    if (cpspec && !rgpspec)
        return E_INVALIDARG;
    if (!(This->grfMode & STGM_READWRITE))
        return STG_E_ACCESSDENIED;
    hr = S_OK;
    EnterCriticalSection(&This->cs);
    This->dirty = TRUE;
    for (i = 0; i < cpspec; i++)
    {
        if (rgpspec[i].ulKind == PRSPEC_LPWSTR)
        {
            PROPID propid;

            if (dictionary_find(This->name_to_propid,
             (void *)rgpspec[i].u.lpwstr, (void **)&propid))
                dictionary_remove(This->propid_to_prop, (void *)propid);
        }
        else
        {
            if (rgpspec[i].u.propid >= PID_FIRST_USABLE &&
             rgpspec[i].u.propid < PID_MIN_READONLY)
                dictionary_remove(This->propid_to_prop,
                 (void *)rgpspec[i].u.propid);
            else
                hr = STG_E_INVALIDPARAMETER;
        }
    }
    if (This->grfFlags & PROPSETFLAG_UNBUFFERED)
        IPropertyStorage_Commit(iface, STGC_DEFAULT);
    LeaveCriticalSection(&This->cs);
    return hr;
}

/************************************************************************
 * IPropertyStorage_fnReadPropertyNames (IPropertyStorage)
 */
static HRESULT WINAPI IPropertyStorage_fnReadPropertyNames(
    IPropertyStorage* iface,
    ULONG cpropid,
    const PROPID rgpropid[],
    LPOLESTR rglpwstrName[])
{
    PropertyStorage_impl *This = (PropertyStorage_impl *)iface;
    ULONG i;
    HRESULT hr = S_FALSE;

    TRACE("(%p, %ld, %p, %p)\n", iface, cpropid, rgpropid, rglpwstrName);
    if (!This)
        return E_INVALIDARG;
    if (cpropid && (!rgpropid || !rglpwstrName))
        return E_INVALIDARG;
    EnterCriticalSection(&This->cs);
    for (i = 0; i < cpropid && SUCCEEDED(hr); i++)
    {
        LPWSTR name = PropertyStorage_FindPropertyNameById(This, rgpropid[i]);

        if (name)
        {
            size_t len = lstrlenW(name);

            hr = S_OK;
            rglpwstrName[i] = CoTaskMemAlloc((len + 1) * sizeof(WCHAR));
            if (rglpwstrName)
                memcpy(rglpwstrName, name, (len + 1) * sizeof(WCHAR));
            else
                hr = STG_E_INSUFFICIENTMEMORY;
        }
        else
            rglpwstrName[i] = NULL;
    }
    LeaveCriticalSection(&This->cs);
    return hr;
}

/************************************************************************
 * IPropertyStorage_fnWritePropertyNames (IPropertyStorage)
 */
static HRESULT WINAPI IPropertyStorage_fnWritePropertyNames(
    IPropertyStorage* iface,
    ULONG cpropid,
    const PROPID rgpropid[],
    const LPOLESTR rglpwstrName[])
{
    PropertyStorage_impl *This = (PropertyStorage_impl *)iface;
    ULONG i;
    HRESULT hr;

    TRACE("(%p, %ld, %p, %p)\n", iface, cpropid, rgpropid, rglpwstrName);
    if (!This)
        return E_INVALIDARG;
    if (cpropid && (!rgpropid || !rglpwstrName))
        return E_INVALIDARG;
    if (!(This->grfMode & STGM_READWRITE))
        return STG_E_ACCESSDENIED;
    hr = S_OK;
    EnterCriticalSection(&This->cs);
    This->dirty = TRUE;
    for (i = 0; i < cpropid; i++)
    {
        if (rgpropid[i] != PID_ILLEGAL)
        {
            size_t len = lstrlenW(rglpwstrName[i]) + 1;
            LPWSTR name = HeapAlloc(GetProcessHeap(), 0, len * sizeof(WCHAR));

            strcpyW(name, rglpwstrName[i]);
            dictionary_insert(This->name_to_propid, name, (void *)rgpropid[i]);
            dictionary_insert(This->propid_to_name, (void *)rgpropid[i], name);
        }
    }
    if (This->grfFlags & PROPSETFLAG_UNBUFFERED)
        IPropertyStorage_Commit(iface, STGC_DEFAULT);
    LeaveCriticalSection(&This->cs);
    return hr;
}

/************************************************************************
 * IPropertyStorage_fnDeletePropertyNames (IPropertyStorage)
 */
static HRESULT WINAPI IPropertyStorage_fnDeletePropertyNames(
    IPropertyStorage* iface,
    ULONG cpropid,
    const PROPID rgpropid[])
{
    PropertyStorage_impl *This = (PropertyStorage_impl *)iface;
    ULONG i;
    HRESULT hr;

    TRACE("(%p, %ld, %p)\n", iface, cpropid, rgpropid);
    if (!This)
        return E_INVALIDARG;
    if (cpropid && !rgpropid)
        return E_INVALIDARG;
    if (!(This->grfMode & STGM_READWRITE))
        return STG_E_ACCESSDENIED;
    hr = S_OK;
    EnterCriticalSection(&This->cs);
    This->dirty = TRUE;
    for (i = 0; i < cpropid; i++)
    {
        LPWSTR name = NULL;

        if (dictionary_find(This->propid_to_name, (void *)rgpropid[i],
         (void **)&name))
        {
            dictionary_remove(This->propid_to_name, (void *)rgpropid[i]);
            dictionary_remove(This->name_to_propid, name);
        }
    }
    if (This->grfFlags & PROPSETFLAG_UNBUFFERED)
        IPropertyStorage_Commit(iface, STGC_DEFAULT);
    LeaveCriticalSection(&This->cs);
    return hr;
}

/************************************************************************
 * IPropertyStorage_fnCommit (IPropertyStorage)
 */
static HRESULT WINAPI IPropertyStorage_fnCommit(
    IPropertyStorage* iface,
    DWORD grfCommitFlags)
{
    PropertyStorage_impl *This = (PropertyStorage_impl *)iface;
    HRESULT hr;

    TRACE("(%p, 0x%08lx)\n", iface, grfCommitFlags);
    if (!This)
        return E_INVALIDARG;
    if (!(This->grfMode & STGM_READWRITE))
        return STG_E_ACCESSDENIED;
    EnterCriticalSection(&This->cs);
    if (This->dirty)
        hr = PropertyStorage_WriteToStream(This);
    else
        hr = S_OK;
    LeaveCriticalSection(&This->cs);
    return hr;
}

/************************************************************************
 * IPropertyStorage_fnRevert (IPropertyStorage)
 */
static HRESULT WINAPI IPropertyStorage_fnRevert(
    IPropertyStorage* iface)
{
    HRESULT hr;
    PropertyStorage_impl *This = (PropertyStorage_impl *)iface;

    TRACE("%p\n", iface);
    if (!This)
        return E_INVALIDARG;

    EnterCriticalSection(&This->cs);
    if (This->dirty)
    {
        PropertyStorage_DestroyDictionaries(This);
        hr = PropertyStorage_CreateDictionaries(This);
        if (SUCCEEDED(hr))
            hr = PropertyStorage_ReadFromStream(This);
    }
    else
        hr = S_OK;
    LeaveCriticalSection(&This->cs);
    return hr;
}

/************************************************************************
 * IPropertyStorage_fnEnum (IPropertyStorage)
 */
static HRESULT WINAPI IPropertyStorage_fnEnum(
    IPropertyStorage* iface,
    IEnumSTATPROPSTG** ppenum)
{
    FIXME("\n");
    return E_NOTIMPL;
}

/************************************************************************
 * IPropertyStorage_fnSetTimes (IPropertyStorage)
 */
static HRESULT WINAPI IPropertyStorage_fnSetTimes(
    IPropertyStorage* iface,
    const FILETIME* pctime,
    const FILETIME* patime,
    const FILETIME* pmtime)
{
    FIXME("\n");
    return E_NOTIMPL;
}

/************************************************************************
 * IPropertyStorage_fnSetClass (IPropertyStorage)
 */
static HRESULT WINAPI IPropertyStorage_fnSetClass(
    IPropertyStorage* iface,
    REFCLSID clsid)
{
    PropertyStorage_impl *This = (PropertyStorage_impl *)iface;

    TRACE("%p, %s\n", iface, debugstr_guid(clsid));
    if (!This || !clsid)
        return E_INVALIDARG;
    if (!(This->grfMode & STGM_READWRITE))
        return STG_E_ACCESSDENIED;
    memcpy(&This->clsid, clsid, sizeof(This->clsid));
    This->dirty = TRUE;
    if (This->grfFlags & PROPSETFLAG_UNBUFFERED)
        IPropertyStorage_Commit(iface, STGC_DEFAULT);
    return S_OK;
}

/************************************************************************
 * IPropertyStorage_fnStat (IPropertyStorage)
 */
static HRESULT WINAPI IPropertyStorage_fnStat(
    IPropertyStorage* iface,
    STATPROPSETSTG* statpsstg)
{
    PropertyStorage_impl *This = (PropertyStorage_impl *)iface;
    STATSTG stat;
    HRESULT hr;

    TRACE("%p, %p\n", iface, statpsstg);
    if (!This || !statpsstg)
        return E_INVALIDARG;

    hr = IStream_Stat(This->stm, &stat, STATFLAG_NONAME);
    if (SUCCEEDED(hr))
    {
        memcpy(&statpsstg->fmtid, &This->fmtid, sizeof(statpsstg->fmtid));
        memcpy(&statpsstg->clsid, &This->clsid, sizeof(statpsstg->clsid));
        statpsstg->grfFlags = This->grfFlags;
        memcpy(&statpsstg->mtime, &stat.mtime, sizeof(statpsstg->mtime));
        memcpy(&statpsstg->ctime, &stat.ctime, sizeof(statpsstg->ctime));
        memcpy(&statpsstg->atime, &stat.atime, sizeof(statpsstg->atime));
        statpsstg->dwOSVersion = This->originatorOS;
    }
    return hr;
}

static int PropertyStorage_PropNameCompare(const void *a, const void *b,
 void *extra)
{
    PropertyStorage_impl *This = (PropertyStorage_impl *)extra;

    TRACE("(%s, %s)\n", debugstr_w(a), debugstr_w(b));
    /* FIXME: this assumes property names are always Unicode, but they
     * might be ANSI, depending on whether This->grfFlags & PROPSETFLAG_ANSI
     * is true.
     */
    if (This->grfFlags & PROPSETFLAG_CASE_SENSITIVE)
        return strcmpW((LPCWSTR)a, (LPCWSTR)b);
    else
        return strcmpiW((LPCWSTR)a, (LPCWSTR)b);
}

static void PropertyStorage_PropNameDestroy(void *k, void *d, void *extra)
{
    HeapFree(GetProcessHeap(), 0, k);
}

static int PropertyStorage_PropCompare(const void *a, const void *b,
 void *extra)
{
    TRACE("(%ld, %ld)\n", (PROPID)a, (PROPID)b);
    return (PROPID)a - (PROPID)b;
}

static void PropertyStorage_PropertyDestroy(void *k, void *d, void *extra)
{
    PropVariantClear((PROPVARIANT *)d);
    HeapFree(GetProcessHeap(), 0, d);
}

/* Reads the dictionary from the memory buffer beginning at ptr.  Interprets
 * the entries according to the values of This->codePage and This->locale.
 * FIXME: there isn't any checking whether the read property extends past the
 * end of the buffer.
 * FIXME: this always stores dictionary entries as Unicode, but it should store
 * them as ANSI if (This->grfFlags & PROPSETFLAG_ANSI) is true.
 */
static HRESULT PropertyStorage_ReadDictionary(PropertyStorage_impl *This,
 BYTE *ptr)
{
    DWORD numEntries, i;
    HRESULT hr = S_OK;

    assert(This);
    assert(This->name_to_propid);
    assert(This->propid_to_name);

    StorageUtl_ReadDWord(ptr, 0, &numEntries);
    TRACE("Reading %ld entries:\n", numEntries);
    ptr += sizeof(DWORD);
    for (i = 0; SUCCEEDED(hr) && i < numEntries; i++)
    {
        PROPID propid;
        DWORD cbEntry;
        LPWSTR name = NULL;

        StorageUtl_ReadDWord(ptr, 0, &propid);
        ptr += sizeof(PROPID);
        StorageUtl_ReadDWord(ptr, 0, &cbEntry);
        ptr += sizeof(DWORD);
        /* FIXME: if host is big-endian, this'll suck to convert */
        TRACE("Reading entry with ID 0x%08lx, %ld bytes\n", propid, cbEntry);
        if (This->codePage != CP_UNICODE)
        {
            int len = MultiByteToWideChar(This->codePage, 0, ptr, cbEntry,
             NULL, 0);

            if (!len)
                hr = HRESULT_FROM_WIN32(GetLastError());
            else
            {
                name = HeapAlloc(GetProcessHeap(), 0,
                 len * sizeof(WCHAR));
                if (name)
                    MultiByteToWideChar(This->codePage, 0, ptr, cbEntry, name,
                     len);
                else
                    hr = STG_E_INSUFFICIENTMEMORY;
            }
        }
        else
        {
            name = HeapAlloc(GetProcessHeap(), 0, cbEntry);
            if (name)
                lstrcpyW(name, (LPWSTR)ptr);
            else
                hr = STG_E_INSUFFICIENTMEMORY;
            /* Unicode entries are padded to DWORD boundaries */
            if (cbEntry % sizeof(DWORD))
                ptr += sizeof(DWORD) - (cbEntry % sizeof(DWORD));
        }
        if (name)
        {
            dictionary_insert(This->name_to_propid, name, (void *)propid);
            dictionary_insert(This->propid_to_name, (void *)propid, name);
            TRACE("Property %s maps to id %ld\n", debugstr_w(name), propid);
        }
        ptr += sizeof(DWORD) + cbEntry;
    }
    return hr;
}

/* FIXME: there isn't any checking whether the read property extends past the
 * end of the buffer.
 */
static HRESULT PropertyStorage_ReadProperty(PROPVARIANT *prop, const BYTE *data)
{
    HRESULT hr = S_OK;

    assert(prop);
    assert(data);
    StorageUtl_ReadDWord(data, 0, (DWORD *)&prop->vt);
    data += sizeof(DWORD);
    switch (prop->vt)
    {
    case VT_EMPTY:
    case VT_NULL:
        break;
    case VT_I1:
        prop->u.cVal = *(const char *)data;
        TRACE("Read char 0x%x ('%c')\n", prop->u.cVal, prop->u.cVal);
        break;
    case VT_UI1:
        prop->u.bVal = *(const UCHAR *)data;
        TRACE("Read byte 0x%x\n", prop->u.bVal);
        break;
    case VT_I2:
        StorageUtl_ReadWord(data, 0, &prop->u.iVal);
        TRACE("Read short %d\n", prop->u.iVal);
        break;
    case VT_UI2:
        StorageUtl_ReadWord(data, 0, &prop->u.uiVal);
        TRACE("Read ushort %d\n", prop->u.uiVal);
        break;
    case VT_I4:
        StorageUtl_ReadDWord(data, 0, &prop->u.lVal);
        TRACE("Read long %ld\n", prop->u.lVal);
        break;
    case VT_UI4:
        StorageUtl_ReadDWord(data, 0, &prop->u.ulVal);
        TRACE("Read ulong %ld\n", prop->u.ulVal);
        break;
    case VT_LPSTR:
    {
        DWORD count;
       
        StorageUtl_ReadDWord(data, 0, &count);
        prop->u.pszVal = CoTaskMemAlloc(count);
        if (prop->u.pszVal)
        {
            /* FIXME: if the host is big-endian, this'll suck */
            memcpy(prop->u.pszVal, data + sizeof(DWORD), count);
            /* FIXME: so far so good, but this may be Unicode or DBCS depending
             * on This->codePage.
             */
            TRACE("Read string value %s\n", debugstr_a(prop->u.pszVal));
        }
        else
            hr = STG_E_INSUFFICIENTMEMORY;
        break;
    }
    case VT_LPWSTR:
    {
        DWORD count;

        StorageUtl_ReadDWord(data, 0, &count);
        prop->u.pwszVal = CoTaskMemAlloc(count * sizeof(WCHAR));
        if (prop->u.pwszVal)
        {
            /* FIXME: if the host is big-endian, gotta swap every char */
            memcpy(prop->u.pwszVal, data + sizeof(DWORD),
             count * sizeof(WCHAR));
            TRACE("Read string value %s\n", debugstr_w(prop->u.pwszVal));
        }
        else
            hr = STG_E_INSUFFICIENTMEMORY;
        break;
    }
    case VT_FILETIME:
        /* FIXME: endianness */
        memcpy(&prop->u.filetime, data, sizeof(FILETIME));
        break;
    default:
        FIXME("unsupported type %d\n", prop->vt);
        hr = STG_E_INVALIDPARAMETER;
    }
    return hr;
}

static HRESULT PropertyStorage_ReadHeaderFromStream(IStream *stm,
 PROPERTYSETHEADER *hdr)
{
    BYTE buf[sizeof(PROPERTYSETHEADER)];
    ULONG count = 0;
    HRESULT hr;

    assert(stm);
    assert(hdr);
    hr = IStream_Read(stm, buf, sizeof(buf), &count);
    if (SUCCEEDED(hr))
    {
        if (count != sizeof(buf))
        {
            WARN("read %ld, expected %d\n", count, sizeof(buf));
            hr = STG_E_INVALIDHEADER;
        }
        else
        {
            StorageUtl_ReadWord(buf, offsetof(PROPERTYSETHEADER, wByteOrder),
             &hdr->wByteOrder);
            StorageUtl_ReadWord(buf, offsetof(PROPERTYSETHEADER, wFormat),
             &hdr->wFormat);
            StorageUtl_ReadDWord(buf, offsetof(PROPERTYSETHEADER, dwOSVer),
             &hdr->dwOSVer);
            StorageUtl_ReadGUID(buf, offsetof(PROPERTYSETHEADER, clsid),
             &hdr->clsid);
            StorageUtl_ReadDWord(buf, offsetof(PROPERTYSETHEADER, reserved),
             &hdr->reserved);
        }
    }
    TRACE("returning 0x%08lx\n", hr);
    return hr;
}

static HRESULT PropertyStorage_ReadFmtIdOffsetFromStream(IStream *stm,
 FORMATIDOFFSET *fmt)
{
    BYTE buf[sizeof(FORMATIDOFFSET)];
    ULONG count = 0;
    HRESULT hr;

    assert(stm);
    assert(fmt);
    hr = IStream_Read(stm, buf, sizeof(buf), &count);
    if (SUCCEEDED(hr))
    {
        if (count != sizeof(buf))
        {
            WARN("read %ld, expected %d\n", count, sizeof(buf));
            hr = STG_E_INVALIDHEADER;
        }
        else
        {
            StorageUtl_ReadGUID(buf, offsetof(FORMATIDOFFSET, fmtid),
             &fmt->fmtid);
            StorageUtl_ReadDWord(buf, offsetof(FORMATIDOFFSET, dwOffset),
             &fmt->dwOffset);
        }
    }
    TRACE("returning 0x%08lx\n", hr);
    return hr;
}

static HRESULT PropertyStorage_ReadSectionHeaderFromStream(IStream *stm,
 PROPERTYSECTIONHEADER *hdr)
{
    BYTE buf[sizeof(PROPERTYSECTIONHEADER)];
    ULONG count = 0;
    HRESULT hr;

    assert(stm);
    assert(hdr);
    hr = IStream_Read(stm, buf, sizeof(buf), &count);
    if (SUCCEEDED(hr))
    {
        if (count != sizeof(buf))
        {
            WARN("read %ld, expected %d\n", count, sizeof(buf));
            hr = STG_E_INVALIDHEADER;
        }
        else
        {
            StorageUtl_ReadDWord(buf, offsetof(PROPERTYSECTIONHEADER,
             cbSection), &hdr->cbSection);
            StorageUtl_ReadDWord(buf, offsetof(PROPERTYSECTIONHEADER,
             cProperties), &hdr->cProperties);
        }
    }
    TRACE("returning 0x%08lx\n", hr);
    return hr;
}

static HRESULT PropertyStorage_ReadFromStream(PropertyStorage_impl *This)
{
    PROPERTYSETHEADER hdr;
    FORMATIDOFFSET fmtOffset;
    PROPERTYSECTIONHEADER sectionHdr;
    LARGE_INTEGER seek;
    ULONG i;
    STATSTG stat;
    HRESULT hr;
    BYTE *buf = NULL;
    ULONG count = 0;
    DWORD dictOffset = 0;

    assert(This);
    This->dirty = FALSE;
    This->highestProp = 0;
    hr = IStream_Stat(This->stm, &stat, STATFLAG_NONAME);
    if (FAILED(hr))
        goto end;
    if (stat.cbSize.u.HighPart)
    {
        WARN("stream too big\n");
        /* maximum size varies, but it can't be this big */
        hr = STG_E_INVALIDHEADER;
        goto end;
    }
    if (stat.cbSize.u.LowPart == 0)
    {
        /* empty stream is okay */
        hr = S_OK;
        goto end;
    }
    else if (stat.cbSize.u.LowPart < sizeof(PROPERTYSETHEADER) +
     sizeof(FORMATIDOFFSET))
    {
        WARN("stream too small\n");
        hr = STG_E_INVALIDHEADER;
        goto end;
    }
    seek.QuadPart = 0;
    hr = IStream_Seek(This->stm, seek, STREAM_SEEK_SET, NULL);
    if (FAILED(hr))
        goto end;
    hr = PropertyStorage_ReadHeaderFromStream(This->stm, &hdr);
    /* I've only seen reserved == 1, but the article says I shouldn't disallow
     * higher values.
     */
    if (hdr.wByteOrder != PROPSETHDR_BYTEORDER_MAGIC || hdr.reserved < 1)
    {
        WARN("bad magic in prop set header\n");
        hr = STG_E_INVALIDHEADER;
        goto end;
    }
    if (hdr.wFormat != 0 && hdr.wFormat != 1)
    {
        WARN("bad format version %d\n", hdr.wFormat);
        hr = STG_E_INVALIDHEADER;
        goto end;
    }
    memcpy(&This->clsid, &hdr.clsid, sizeof(This->clsid));
    This->originatorOS = hdr.dwOSVer;
    if (PROPSETHDR_OSVER_KIND(hdr.dwOSVer) == PROPSETHDR_OSVER_KIND_MAC)
        WARN("File comes from a Mac, strings will probably be screwed up\n");
    hr = PropertyStorage_ReadFmtIdOffsetFromStream(This->stm, &fmtOffset);
    if (FAILED(hr))
        goto end;
    if (fmtOffset.dwOffset > stat.cbSize.u.LowPart)
    {
        WARN("invalid offset %ld (stream length is %ld)\n", fmtOffset.dwOffset,
         stat.cbSize.u.LowPart);
        hr = STG_E_INVALIDHEADER;
        goto end;
    }
    /* wackiness alert: if the format ID is FMTID_DocSummaryInformation, there
     * follow not one, but two sections.  The first is the standard properties
     * for the document summary information, and the second is user-defined
     * properties.  This is the only case in which multiple sections are
     * allowed.
     * Reading the second stream isn't implemented yet.
     */
    hr = PropertyStorage_ReadSectionHeaderFromStream(This->stm, &sectionHdr);
    if (FAILED(hr))
        goto end;
    /* The section size includes the section header, so check it */
    if (sectionHdr.cbSection < sizeof(PROPERTYSECTIONHEADER))
    {
        WARN("section header too small, got %ld, expected at least %d\n",
         sectionHdr.cbSection, sizeof(PROPERTYSECTIONHEADER));
        hr = STG_E_INVALIDHEADER;
        goto end;
    }
    buf = HeapAlloc(GetProcessHeap(), 0, sectionHdr.cbSection -
     sizeof(PROPERTYSECTIONHEADER));
    if (!buf)
    {
        hr = STG_E_INSUFFICIENTMEMORY;
        goto end;
    }
    hr = IStream_Read(This->stm, buf, sectionHdr.cbSection -
     sizeof(PROPERTYSECTIONHEADER), &count);
    if (FAILED(hr))
        goto end;
    TRACE("Reading %ld properties:\n", sectionHdr.cProperties);
    for (i = 0; SUCCEEDED(hr) && i < sectionHdr.cProperties; i++)
    {
        PROPERTYIDOFFSET *idOffset = (PROPERTYIDOFFSET *)(buf +
         i * sizeof(PROPERTYIDOFFSET));

        if (idOffset->dwOffset < sizeof(PROPERTYSECTIONHEADER) ||
         idOffset->dwOffset >= sectionHdr.cbSection - sizeof(DWORD))
            hr = STG_E_INVALIDPOINTER;
        else
        {
            if (idOffset->propid >= PID_FIRST_USABLE &&
             idOffset->propid < PID_MIN_READONLY && idOffset->propid >
             This->highestProp)
                This->highestProp = idOffset->propid;
            if (idOffset->propid == PID_DICTIONARY)
            {
                /* Don't read the dictionary yet, its entries depend on the
                 * code page.  Just store the offset so we know to read it
                 * later.
                 */
                dictOffset = idOffset->dwOffset;
                TRACE("Dictionary offset is %ld\n", dictOffset);
            }
            else
            {
                PROPVARIANT prop;

                if (SUCCEEDED(PropertyStorage_ReadProperty(&prop,
                 buf + idOffset->dwOffset - sizeof(PROPERTYSECTIONHEADER))))
                {
                    TRACE("Read property with ID 0x%08lx, type %d\n",
                     idOffset->propid, prop.vt);
                    switch(idOffset->propid)
                    {
                    case PID_CODEPAGE:
                        if (prop.vt == VT_I2)
                            This->codePage = (UINT)prop.u.iVal;
                        break;
                    case PID_LOCALE:
                        if (prop.vt == VT_I4)
                            This->locale = (LCID)prop.u.lVal;
                        break;
                    case PID_BEHAVIOR:
                        if (prop.vt == VT_I4 && prop.u.lVal)
                            This->grfFlags |= PROPSETFLAG_CASE_SENSITIVE;
                        break;
                    default:
                        hr = PropertyStorage_StorePropWithId(This,
                         idOffset->propid, &prop);
                    }
                }
            }
        }
    }
    if (!This->codePage)
    {
        /* default to Unicode unless told not to, as specified here:
         * http://msdn.microsoft.com/library/en-us/stg/stg/names_in_istorage.asp
         */
        if (This->grfFlags & PROPSETFLAG_ANSI)
            This->codePage = GetACP();
        else
            This->codePage = CP_UNICODE;
    }
    if (!This->locale)
        This->locale = LOCALE_SYSTEM_DEFAULT;
    TRACE("Code page is %d, locale is %ld\n", This->codePage, This->locale);
    if (dictOffset)
        hr = PropertyStorage_ReadDictionary(This,
         buf + dictOffset - sizeof(PROPERTYSECTIONHEADER));

end:
    HeapFree(GetProcessHeap(), 0, buf);
    if (FAILED(hr))
    {
        dictionary_destroy(This->name_to_propid);
        This->name_to_propid = NULL;
        dictionary_destroy(This->propid_to_name);
        This->propid_to_name = NULL;
        dictionary_destroy(This->propid_to_prop);
        This->propid_to_prop = NULL;
    }
    return hr;
}

static void PropertyStorage_MakeHeader(PropertyStorage_impl *This,
 PROPERTYSETHEADER *hdr)
{
    assert(This);
    assert(hdr);
    StorageUtl_WriteWord((BYTE *)&hdr->wByteOrder, 0,
     PROPSETHDR_BYTEORDER_MAGIC);
    /* FIXME: should be able to write format 0 property sets too, depending
     * on whether I have too long string names or if case-sensitivity is set.
     * For now always write format 1.
     */
    StorageUtl_WriteWord((BYTE *)&hdr->wFormat, 0, 1);
    StorageUtl_WriteDWord((BYTE *)&hdr->dwOSVer, 0, This->originatorOS);
    StorageUtl_WriteGUID((BYTE *)&hdr->clsid, 0, &This->clsid);
    StorageUtl_WriteDWord((BYTE *)&hdr->reserved, 0, 1);
}

static void PropertyStorage_MakeFmtIdOffset(PropertyStorage_impl *This,
 FORMATIDOFFSET *fmtOffset)
{
    assert(This);
    assert(fmtOffset);
    StorageUtl_WriteGUID((BYTE *)fmtOffset, 0, &This->fmtid);
    StorageUtl_WriteDWord((BYTE *)fmtOffset, offsetof(FORMATIDOFFSET, dwOffset),
     sizeof(PROPERTYSETHEADER) + sizeof(FORMATIDOFFSET));
}

static void PropertyStorage_MakeSectionHdr(DWORD cbSection, DWORD numProps,
 PROPERTYSECTIONHEADER *hdr)
{
    assert(hdr);
    StorageUtl_WriteDWord((BYTE *)hdr, 0, cbSection);
    StorageUtl_WriteDWord((BYTE *)hdr,
     offsetof(PROPERTYSECTIONHEADER, cProperties), numProps);
}

static void PropertyStorage_MakePropertyIdOffset(DWORD propid, DWORD dwOffset,
 PROPERTYIDOFFSET *propIdOffset)
{
    assert(propIdOffset);
    StorageUtl_WriteDWord((BYTE *)propIdOffset, 0, propid);
    StorageUtl_WriteDWord((BYTE *)propIdOffset,
     offsetof(PROPERTYIDOFFSET, dwOffset), dwOffset);
}

struct DictionaryClosure
{
    HRESULT hr;
    DWORD bytesWritten;
};

static BOOL PropertyStorage_DictionaryWriter(const void *key,
 const void *value, void *extra, void *closure)
{
    PropertyStorage_impl *This = (PropertyStorage_impl *)extra;
    struct DictionaryClosure *c = (struct DictionaryClosure *)closure;
    DWORD propid;
    ULONG count;

    assert(key);
    assert(This);
    assert(closure);
    StorageUtl_WriteDWord((LPBYTE)&propid, 0, (DWORD)value);
    c->hr = IStream_Write(This->stm, &propid, sizeof(propid), &count);
    if (FAILED(c->hr))
        goto end;
    c->bytesWritten += sizeof(DWORD);
    if (This->codePage == CP_UNICODE)
    {
        DWORD keyLen, pad = 0;

        StorageUtl_WriteDWord((LPBYTE)&keyLen, 0,
         (lstrlenW((LPWSTR)key) + 1) * sizeof(WCHAR));
        c->hr = IStream_Write(This->stm, &keyLen, sizeof(keyLen), &count);
        if (FAILED(c->hr))
            goto end;
        c->bytesWritten += sizeof(DWORD);
        /* FIXME: endian-convert every char (yuck) */
        c->hr = IStream_Write(This->stm, key, keyLen, &count);
        if (FAILED(c->hr))
            goto end;
        c->bytesWritten += keyLen;
        if (keyLen % sizeof(DWORD))
        {
            c->hr = IStream_Write(This->stm, &pad,
             sizeof(DWORD) - keyLen % sizeof(DWORD), &count);
            if (FAILED(c->hr))
                goto end;
            c->bytesWritten += sizeof(DWORD) - keyLen % sizeof(DWORD);
        }
    }
    else
    {
        int len = WideCharToMultiByte(This->codePage, 0, (LPWSTR)key, -1, NULL,
         0, NULL, NULL);
        LPBYTE buf = HeapAlloc(GetProcessHeap(), 0, len);
        DWORD dwLen;

        if (!buf)
        {
            c->hr = STG_E_INSUFFICIENTMEMORY;
            goto end;
        }
        /* FIXME: endian-convert multibyte chars?  Ick! */
        WideCharToMultiByte(This->codePage, 0, (LPWSTR)key, -1, buf, len,
         NULL, NULL);
        StorageUtl_WriteDWord((LPBYTE)&dwLen, 0, len);
        c->hr = IStream_Write(This->stm, &dwLen, sizeof(dwLen), &count);
        if (FAILED(c->hr))
        {
            HeapFree(GetProcessHeap(), 0, buf);
            goto end;
        }
        c->bytesWritten += sizeof(DWORD);
        c->hr = IStream_Write(This->stm, buf, len, &count);
        if (FAILED(c->hr))
        {
            HeapFree(GetProcessHeap(), 0, buf);
            goto end;
        }
        c->bytesWritten += len;
        HeapFree(GetProcessHeap(), 0, buf);
    }
end:
    return SUCCEEDED(c->hr);
}

#define SECTIONHEADER_OFFSET sizeof(PROPERTYSETHEADER) + sizeof(FORMATIDOFFSET)

/* Writes the dictionary to the stream.  Assumes without checking that the
 * dictionary isn't empty.
 */
static HRESULT PropertyStorage_WriteDictionaryToStream(
 PropertyStorage_impl *This, DWORD *sectionOffset)
{
    HRESULT hr;
    LARGE_INTEGER seek;
    PROPERTYIDOFFSET propIdOffset;
    ULONG count;
    DWORD dwTemp;
    struct DictionaryClosure closure;

    assert(This);
    assert(sectionOffset);

    /* The dictionary's always the first property written, so seek to its
     * spot.
     */
    seek.QuadPart = SECTIONHEADER_OFFSET + sizeof(PROPERTYSECTIONHEADER);
    hr = IStream_Seek(This->stm, seek, STREAM_SEEK_SET, NULL);
    if (FAILED(hr))
        goto end;
    PropertyStorage_MakePropertyIdOffset(PID_DICTIONARY, *sectionOffset,
     &propIdOffset);
    hr = IStream_Write(This->stm, &propIdOffset, sizeof(propIdOffset), &count);
    if (FAILED(hr))
        goto end;

    seek.QuadPart = SECTIONHEADER_OFFSET + *sectionOffset;
    hr = IStream_Seek(This->stm, seek, STREAM_SEEK_SET, NULL);
    if (FAILED(hr))
        goto end;
    StorageUtl_WriteDWord((LPBYTE)&dwTemp, 0,
     dictionary_num_entries(This->name_to_propid));
    hr = IStream_Write(This->stm, &dwTemp, sizeof(dwTemp), &count);
    if (FAILED(hr))
        goto end;
    *sectionOffset += sizeof(dwTemp);

    closure.hr = S_OK;
    closure.bytesWritten = 0;
    dictionary_enumerate(This->name_to_propid, PropertyStorage_DictionaryWriter,
     &closure);
    hr = closure.hr;
    if (FAILED(hr))
        goto end;
    *sectionOffset += closure.bytesWritten;
    if (closure.bytesWritten % sizeof(DWORD))
    {
        TRACE("adding %ld bytes of padding\n", sizeof(DWORD) -
         closure.bytesWritten % sizeof(DWORD));
        *sectionOffset += sizeof(DWORD) - closure.bytesWritten % sizeof(DWORD);
    }

end:
    return hr;
}

static HRESULT PropertyStorage_WritePropertyToStream(PropertyStorage_impl *This,
 DWORD propNum, DWORD propid, PROPVARIANT *var, DWORD *sectionOffset)
{
    HRESULT hr;
    LARGE_INTEGER seek;
    PROPERTYIDOFFSET propIdOffset;
    ULONG count;
    DWORD dwType, bytesWritten;

    assert(This);
    assert(var);
    assert(sectionOffset);

    TRACE("%p, %ld, 0x%08lx, (%d), (%ld)\n", This, propNum, propid, var->vt,
     *sectionOffset);

    seek.QuadPart = SECTIONHEADER_OFFSET + sizeof(PROPERTYSECTIONHEADER) +
     propNum * sizeof(PROPERTYIDOFFSET);
    hr = IStream_Seek(This->stm, seek, STREAM_SEEK_SET, NULL);
    if (FAILED(hr))
        goto end;
    PropertyStorage_MakePropertyIdOffset(propid, *sectionOffset, &propIdOffset);
    hr = IStream_Write(This->stm, &propIdOffset, sizeof(propIdOffset), &count);
    if (FAILED(hr))
        goto end;

    seek.QuadPart = SECTIONHEADER_OFFSET + *sectionOffset;
    hr = IStream_Seek(This->stm, seek, STREAM_SEEK_SET, NULL);
    if (FAILED(hr))
        goto end;
    StorageUtl_WriteDWord((LPBYTE)&dwType, 0, var->vt);
    hr = IStream_Write(This->stm, &dwType, sizeof(dwType), &count);
    if (FAILED(hr))
        goto end;
    *sectionOffset += sizeof(dwType);

    switch (var->vt)
    {
    case VT_EMPTY:
    case VT_NULL:
        bytesWritten = 0;
        break;
    case VT_I1:
    case VT_UI1:
        hr = IStream_Write(This->stm, &var->u.cVal, sizeof(var->u.cVal),
         &count);
        bytesWritten = count;
        break;
    case VT_I2:
    case VT_UI2:
    {
        WORD wTemp;

        StorageUtl_WriteWord((LPBYTE)&wTemp, 0, var->u.iVal);
        hr = IStream_Write(This->stm, &wTemp, sizeof(wTemp), &count);
        bytesWritten = count;
        break;
    }
    case VT_I4:
    case VT_UI4:
    {
        DWORD dwTemp;

        StorageUtl_WriteDWord((LPBYTE)&dwTemp, 0, var->u.lVal);
        hr = IStream_Write(This->stm, &dwTemp, sizeof(dwTemp), &count);
        bytesWritten = count;
        break;
    }
    case VT_LPSTR:
    {
        DWORD len, dwTemp;

        if (This->codePage == CP_UNICODE)
            len = (lstrlenW(var->u.pwszVal) + 1) * sizeof(WCHAR);
        else
            len = lstrlenA(var->u.pszVal) + 1;
        StorageUtl_WriteDWord((LPBYTE)&dwTemp, 0, len);
        hr = IStream_Write(This->stm, &dwTemp, sizeof(dwTemp), &count);
        if (FAILED(hr))
            goto end;
        hr = IStream_Write(This->stm, var->u.pszVal, len, &count);
        bytesWritten = count + sizeof(DWORD);
        break;
    }
    case VT_LPWSTR:
    {
        DWORD len = lstrlenW(var->u.pwszVal) + 1, dwTemp;

        StorageUtl_WriteDWord((LPBYTE)&dwTemp, 0, len);
        hr = IStream_Write(This->stm, &dwTemp, sizeof(dwTemp), &count);
        if (FAILED(hr))
            goto end;
        hr = IStream_Write(This->stm, var->u.pwszVal, len * sizeof(WCHAR),
         &count);
        bytesWritten = count + sizeof(DWORD);
        break;
    }
    default:
        FIXME("unsupported type: %d\n", var->vt);
        return STG_E_INVALIDPARAMETER;
    }

    if (SUCCEEDED(hr))
    {
        *sectionOffset += bytesWritten;
        if (bytesWritten % sizeof(DWORD))
        {
            TRACE("adding %ld bytes of padding\n", sizeof(DWORD) -
             bytesWritten % sizeof(DWORD));
            *sectionOffset += sizeof(DWORD) - bytesWritten % sizeof(DWORD);
        }
    }

end:
    return hr;
}

struct PropertyClosure
{
    HRESULT hr;
    DWORD   propNum;
    DWORD  *sectionOffset;
};

static BOOL PropertyStorage_PropertiesWriter(const void *key, const void *value,
 void *extra, void *closure)
{
    PropertyStorage_impl *This = (PropertyStorage_impl *)extra;
    struct PropertyClosure *c = (struct PropertyClosure *)closure;

    assert(key);
    assert(value);
    assert(extra);
    assert(closure);
    c->hr = PropertyStorage_WritePropertyToStream(This,
     c->propNum++, (DWORD)key, (PROPVARIANT *)value, c->sectionOffset);
    return SUCCEEDED(c->hr);
}

static HRESULT PropertyStorage_WritePropertiesToStream(
 PropertyStorage_impl *This, DWORD startingPropNum, DWORD *sectionOffset)
{
    struct PropertyClosure closure;

    assert(This);
    assert(sectionOffset);
    closure.hr = S_OK;
    closure.propNum = startingPropNum;
    closure.sectionOffset = sectionOffset;
    dictionary_enumerate(This->propid_to_prop, PropertyStorage_PropertiesWriter,
     &closure);
    return closure.hr;
}

static HRESULT PropertyStorage_WriteHeadersToStream(PropertyStorage_impl *This)
{
    HRESULT hr;
    ULONG count = 0;
    LARGE_INTEGER seek = { {0} };
    PROPERTYSETHEADER hdr;
    FORMATIDOFFSET fmtOffset;

    assert(This);
    hr = IStream_Seek(This->stm, seek, STREAM_SEEK_SET, NULL);
    if (FAILED(hr))
        goto end;
    PropertyStorage_MakeHeader(This, &hdr);
    hr = IStream_Write(This->stm, &hdr, sizeof(hdr), &count);
    if (FAILED(hr))
        goto end;
    if (count != sizeof(hdr))
    {
        hr = STG_E_WRITEFAULT;
        goto end;
    }

    PropertyStorage_MakeFmtIdOffset(This, &fmtOffset);
    hr = IStream_Write(This->stm, &fmtOffset, sizeof(fmtOffset), &count);
    if (FAILED(hr))
        goto end;
    if (count != sizeof(fmtOffset))
    {
        hr = STG_E_WRITEFAULT;
        goto end;
    }
    hr = S_OK;

end:
    return hr;
}

static HRESULT PropertyStorage_WriteToStream(PropertyStorage_impl *This)
{
    PROPERTYSECTIONHEADER sectionHdr;
    HRESULT hr;
    ULONG count;
    LARGE_INTEGER seek;
    DWORD numProps, prop, sectionOffset, dwTemp;
    PROPVARIANT var;

    assert(This);

    PropertyStorage_WriteHeadersToStream(This);

    /* Count properties.  Always at least one property, the code page */
    numProps = 1;
    if (dictionary_num_entries(This->name_to_propid))
        numProps++;
    if (This->locale != LOCALE_SYSTEM_DEFAULT)
        numProps++;
    if (This->grfFlags & PROPSETFLAG_CASE_SENSITIVE)
        numProps++;
    numProps += dictionary_num_entries(This->propid_to_prop);

    /* Write section header with 0 bytes right now, I'll adjust it after
     * writing properties.
     */
    PropertyStorage_MakeSectionHdr(0, numProps, &sectionHdr);
    seek.QuadPart = SECTIONHEADER_OFFSET;
    hr = IStream_Seek(This->stm, seek, STREAM_SEEK_SET, NULL);
    if (FAILED(hr))
        goto end;
    hr = IStream_Write(This->stm, &sectionHdr, sizeof(sectionHdr), &count);
    if (FAILED(hr))
        goto end;

    prop = 0;
    sectionOffset = sizeof(PROPERTYSECTIONHEADER) +
     numProps * sizeof(PROPERTYIDOFFSET);

    if (dictionary_num_entries(This->name_to_propid))
    {
        prop++;
        hr = PropertyStorage_WriteDictionaryToStream(This, &sectionOffset);
        if (FAILED(hr))
            goto end;
    }

    PropVariantInit(&var);

    var.vt = VT_I2;
    var.u.iVal = This->codePage;
    hr = PropertyStorage_WritePropertyToStream(This, prop++, PID_CODEPAGE,
     &var, &sectionOffset);
    if (FAILED(hr))
        goto end;

    if (This->locale != LOCALE_SYSTEM_DEFAULT)
    {
        var.vt = VT_I4;
        var.u.lVal = This->locale;
        hr = PropertyStorage_WritePropertyToStream(This, prop++, PID_LOCALE,
         &var, &sectionOffset);
        if (FAILED(hr))
            goto end;
    }

    if (This->grfFlags & PROPSETFLAG_CASE_SENSITIVE)
    {
        var.vt = VT_I4;
        var.u.lVal = 1;
        hr = PropertyStorage_WritePropertyToStream(This, prop++, PID_BEHAVIOR,
         &var, &sectionOffset);
        if (FAILED(hr))
            goto end;
    }

    hr = PropertyStorage_WritePropertiesToStream(This, prop, &sectionOffset);
    if (FAILED(hr))
        goto end;

    /* Now write the byte count of the section */
    seek.QuadPart = SECTIONHEADER_OFFSET;
    hr = IStream_Seek(This->stm, seek, STREAM_SEEK_SET, NULL);
    if (FAILED(hr))
        goto end;
    StorageUtl_WriteDWord((LPBYTE)&dwTemp, 0, sectionOffset);
    hr = IStream_Write(This->stm, &dwTemp, sizeof(dwTemp), &count);

end:
    return hr;
}

/***********************************************************************
 * PropertyStorage_Construct
 */
static void PropertyStorage_DestroyDictionaries(PropertyStorage_impl *This)
{
    assert(This);
    dictionary_destroy(This->name_to_propid);
    This->name_to_propid = NULL;
    dictionary_destroy(This->propid_to_name);
    This->propid_to_name = NULL;
    dictionary_destroy(This->propid_to_prop);
    This->propid_to_prop = NULL;
}

static HRESULT PropertyStorage_CreateDictionaries(PropertyStorage_impl *This)
{
    HRESULT hr = S_OK;

    assert(This);
    This->name_to_propid = dictionary_create(
     PropertyStorage_PropNameCompare, PropertyStorage_PropNameDestroy,
     This);
    if (!This->name_to_propid)
    {
        hr = STG_E_INSUFFICIENTMEMORY;
        goto end;
    }
    This->propid_to_name = dictionary_create(PropertyStorage_PropCompare,
     NULL, This);
    if (!This->propid_to_name)
    {
        hr = STG_E_INSUFFICIENTMEMORY;
        goto end;
    }
    This->propid_to_prop = dictionary_create(PropertyStorage_PropCompare,
     PropertyStorage_PropertyDestroy, This);
    if (!This->propid_to_prop)
    {
        hr = STG_E_INSUFFICIENTMEMORY;
        goto end;
    }
end:
    if (FAILED(hr))
        PropertyStorage_DestroyDictionaries(This);
    return hr;
}

static HRESULT PropertyStorage_BaseConstruct(IStream *stm,
 REFFMTID rfmtid, DWORD grfMode, PropertyStorage_impl **pps)
{
    HRESULT hr = S_OK;

    assert(pps);
    assert(rfmtid);
    *pps = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof **pps);
    if (!pps)
        return E_OUTOFMEMORY;

    (*pps)->vtbl = &IPropertyStorage_Vtbl;
    (*pps)->ref = 1;
    InitializeCriticalSection(&(*pps)->cs);
    (*pps)->stm = stm;
    memcpy(&(*pps)->fmtid, rfmtid, sizeof((*pps)->fmtid));
    (*pps)->grfMode = grfMode;

    hr = PropertyStorage_CreateDictionaries(*pps);

    return hr;
}

static HRESULT PropertyStorage_ConstructFromStream(IStream *stm,
 REFFMTID rfmtid, DWORD grfMode, IPropertyStorage** pps)
{
    PropertyStorage_impl *ps;
    HRESULT hr;

    assert(pps);
    hr = PropertyStorage_BaseConstruct(stm, rfmtid, grfMode, &ps);
    if (SUCCEEDED(hr))
    {
        hr = PropertyStorage_ReadFromStream(ps);
        if (SUCCEEDED(hr))
        {
            *pps = (IPropertyStorage *)ps;
            TRACE("PropertyStorage %p constructed\n", ps);
            hr = S_OK;
        }
        else
        {
            PropertyStorage_DestroyDictionaries(ps);
            HeapFree(GetProcessHeap(), 0, ps);
        }
    }
    return hr;
}

static HRESULT PropertyStorage_ConstructEmpty(IStream *stm,
 REFFMTID rfmtid, DWORD grfFlags, DWORD grfMode, IPropertyStorage** pps)
{
    PropertyStorage_impl *ps;
    HRESULT hr;

    assert(pps);
    hr = PropertyStorage_BaseConstruct(stm, rfmtid, grfMode, &ps);
    if (SUCCEEDED(hr))
    {
        ps->grfFlags = grfFlags;
        /* default to Unicode unless told not to, as specified here:
         * http://msdn.microsoft.com/library/en-us/stg/stg/names_in_istorage.asp
         */
        if (ps->grfFlags & PROPSETFLAG_ANSI)
            ps->codePage = GetACP();
        else
            ps->codePage = CP_UNICODE;
        ps->locale = LOCALE_SYSTEM_DEFAULT;
        TRACE("Code page is %d, locale is %ld\n", ps->codePage, ps->locale);
        *pps = (IPropertyStorage *)ps;
        TRACE("PropertyStorage %p constructed\n", ps);
        hr = S_OK;
    }
    return hr;
}


/***********************************************************************
 * Implementation of IPropertySetStorage
 */

#define BITS_PER_BYTE    8
#define CHARMASK         0x1f
#define BITS_IN_CHARMASK 5

/* Converts rfmtid to a string and returns the resulting string.  If rfmtid
 * is a well-known FMTID, it just returns a static string.  Otherwise it
 * creates the appropriate string name in str, which must be 27 characters
 * in length, and returns str.
 * Based on the algorithm described here:
 * http://msdn.microsoft.com/library/en-us/stg/stg/names_in_istorage.asp
 */
static LPCWSTR format_id_to_name(REFFMTID rfmtid, LPWSTR str)
{
    static const char fmtMap[] = "abcdefghijklmnopqrstuvwxyz012345";
    static const WCHAR szSummaryInfo[] = { 5,'S','u','m','m','a','r','y',
        'I','n','f','o','r','m','a','t','i','o','n',0 };
    static const WCHAR szDocSummaryInfo[] = { 5,'D','o','c','u','m','e','n','t',
        'S','u','m','m','a','r','y','I','n','f','o','r','m','a','t','i','o','n',
        0 };
    LPCWSTR ret;

    if (IsEqualGUID(&FMTID_SummaryInformation, rfmtid))
        ret = szSummaryInfo;
    else if (IsEqualGUID(&FMTID_DocSummaryInformation, rfmtid))
        ret = szDocSummaryInfo;
    else
    {
        BYTE *fmtptr;
        WCHAR *pstr = str;
        ULONG bitsRemaining = BITS_PER_BYTE;

        *pstr++ = 5;
        for (fmtptr = (BYTE *)rfmtid; fmtptr < (BYTE *)rfmtid + sizeof(FMTID); )
        {
            ULONG i = *fmtptr >> (BITS_PER_BYTE - bitsRemaining);

            if (bitsRemaining >= BITS_IN_CHARMASK)
            {
                *pstr = (WCHAR)(fmtMap[i & CHARMASK]);
                if (bitsRemaining == BITS_PER_BYTE && *pstr >= 'a' &&
                 *pstr <= 'z')
                    *pstr += 'A' - 'a';
                pstr++;
                bitsRemaining -= BITS_IN_CHARMASK;
                if (bitsRemaining == 0)
                {
                    fmtptr++;
                    bitsRemaining = BITS_PER_BYTE;
                }
            }
            else
            {
                if (++fmtptr < (BYTE *)rfmtid + sizeof(FMTID))
                    i |= *fmtptr << bitsRemaining;
                *pstr++ = (WCHAR)(fmtMap[i & CHARMASK]);
            }
        }
        *pstr = 0;
        ret = str;
    }
    TRACE("returning %s\n", debugstr_w(ret));
    return ret;
}

/************************************************************************
 * IPropertySetStorage_fnQueryInterface (IUnknown)
 *
 *  This method forwards to the common QueryInterface implementation
 */
static HRESULT WINAPI IPropertySetStorage_fnQueryInterface(
    IPropertySetStorage *ppstg,
    REFIID riid,
    void** ppvObject)
{
    _ICOM_THIS_From_IPropertySetStorage(StorageImpl, ppstg);
    return IStorage_QueryInterface( (IStorage*)This, riid, ppvObject );
}

/************************************************************************
 * IPropertySetStorage_fnAddRef (IUnknown)
 *
 *  This method forwards to the common AddRef implementation
 */
static ULONG WINAPI IPropertySetStorage_fnAddRef(
    IPropertySetStorage *ppstg)
{
    _ICOM_THIS_From_IPropertySetStorage(StorageImpl, ppstg);
    return IStorage_AddRef( (IStorage*)This );
}

/************************************************************************
 * IPropertySetStorage_fnRelease (IUnknown)
 *
 *  This method forwards to the common Release implementation
 */
static ULONG WINAPI IPropertySetStorage_fnRelease(
    IPropertySetStorage *ppstg)
{
    _ICOM_THIS_From_IPropertySetStorage(StorageImpl, ppstg);
    return IStorage_Release( (IStorage*)This );
}

/************************************************************************
 * IPropertySetStorage_fnCreate (IPropertySetStorage)
 */
static HRESULT WINAPI IPropertySetStorage_fnCreate(
    IPropertySetStorage *ppstg,
    REFFMTID rfmtid,
    const CLSID* pclsid,
    DWORD grfFlags,
    DWORD grfMode,
    IPropertyStorage** ppprstg)
{
    _ICOM_THIS_From_IPropertySetStorage(StorageImpl, ppstg);
    WCHAR nameBuf[27];
    LPCWSTR name = NULL;
    IStream *stm = NULL;
    HRESULT r;

    TRACE("%p %s %08lx %08lx %p\n", This, debugstr_guid(rfmtid), grfFlags,
     grfMode, ppprstg);

    /* be picky */
    if (grfMode != (STGM_CREATE|STGM_READWRITE|STGM_SHARE_EXCLUSIVE))
    {
        r = STG_E_INVALIDFLAG;
        goto end;
    }

    if (!rfmtid)
    {
        r = E_INVALIDARG;
        goto end;
    }

    /* FIXME: if (grfFlags & PROPSETFLAG_NONSIMPLE), we need to create a
     * storage, not a stream.  For now, disallow it.
     */
    if (grfFlags & PROPSETFLAG_NONSIMPLE)
    {
        FIXME("PROPSETFLAG_NONSIMPLE not supported\n");
        r = STG_E_INVALIDFLAG;
        goto end;
    }

    name = format_id_to_name(rfmtid, nameBuf);

    r = IStorage_CreateStream( (IStorage*)This, name, grfMode, 0, 0, &stm );
    if (FAILED(r))
        goto end;

    r = PropertyStorage_ConstructEmpty(stm, rfmtid, grfFlags, grfMode, ppprstg);

end:
    TRACE("returning 0x%08lx\n", r);
    return r;
}

/************************************************************************
 * IPropertySetStorage_fnOpen (IPropertySetStorage)
 */
static HRESULT WINAPI IPropertySetStorage_fnOpen(
    IPropertySetStorage *ppstg,
    REFFMTID rfmtid,
    DWORD grfMode,
    IPropertyStorage** ppprstg)
{
    _ICOM_THIS_From_IPropertySetStorage(StorageImpl, ppstg);
    IStream *stm = NULL;
    WCHAR nameBuf[27];
    LPCWSTR name = NULL;
    HRESULT r;

    TRACE("%p %s %08lx %p\n", This, debugstr_guid(rfmtid), grfMode, ppprstg);

    /* be picky */
    if (grfMode != (STGM_READWRITE|STGM_SHARE_EXCLUSIVE) &&
        grfMode != (STGM_READ|STGM_SHARE_EXCLUSIVE))
    {
        r = STG_E_INVALIDFLAG;
        goto end;
    }

    if (!rfmtid)
    {
        r = E_INVALIDARG;
        goto end;
    }

    name = format_id_to_name(rfmtid, nameBuf);

    r = IStorage_OpenStream((IStorage*) This, name, 0, grfMode, 0, &stm );
    if (FAILED(r))
        goto end;

    r = PropertyStorage_ConstructFromStream(stm, rfmtid, grfMode, ppprstg);

end:
    TRACE("returning 0x%08lx\n", r);
    return r;
}

/************************************************************************
 * IPropertySetStorage_fnDelete (IPropertySetStorage)
 */
static HRESULT WINAPI IPropertySetStorage_fnDelete(
    IPropertySetStorage *ppstg,
    REFFMTID rfmtid)
{
    _ICOM_THIS_From_IPropertySetStorage(StorageImpl, ppstg);
    IStorage *stg = NULL;
    WCHAR nameBuf[27];
    LPCWSTR name = NULL;

    TRACE("%p %s\n", This, debugstr_guid(rfmtid));

    if (!rfmtid)
        return E_INVALIDARG;

    name = format_id_to_name(rfmtid, nameBuf);
    if (!name)
        return STG_E_FILENOTFOUND;

    stg = (IStorage*) This;
    return IStorage_DestroyElement(stg, name);
}

/************************************************************************
 * IPropertySetStorage_fnEnum (IPropertySetStorage)
 */
static HRESULT WINAPI IPropertySetStorage_fnEnum(
    IPropertySetStorage *ppstg,
    IEnumSTATPROPSETSTG** ppenum)
{
    _ICOM_THIS_From_IPropertySetStorage(StorageImpl, ppstg);
    FIXME("%p\n", This);
    return E_NOTIMPL;
}


/***********************************************************************
 * vtables
 */
IPropertySetStorageVtbl IPropertySetStorage_Vtbl =
{
    IPropertySetStorage_fnQueryInterface,
    IPropertySetStorage_fnAddRef,
    IPropertySetStorage_fnRelease,
    IPropertySetStorage_fnCreate,
    IPropertySetStorage_fnOpen,
    IPropertySetStorage_fnDelete,
    IPropertySetStorage_fnEnum
};

static IPropertyStorageVtbl IPropertyStorage_Vtbl =
{
    IPropertyStorage_fnQueryInterface,
    IPropertyStorage_fnAddRef,
    IPropertyStorage_fnRelease,
    IPropertyStorage_fnReadMultiple,
    IPropertyStorage_fnWriteMultiple,
    IPropertyStorage_fnDeleteMultiple,
    IPropertyStorage_fnReadPropertyNames,
    IPropertyStorage_fnWritePropertyNames,
    IPropertyStorage_fnDeletePropertyNames,
    IPropertyStorage_fnCommit,
    IPropertyStorage_fnRevert,
    IPropertyStorage_fnEnum,
    IPropertyStorage_fnSetTimes,
    IPropertyStorage_fnSetClass,
    IPropertyStorage_fnStat,
};
