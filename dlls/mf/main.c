/*
 * Copyright (C) 2015 Austin English
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

#include "config.h"

#include <stdarg.h>

#define COBJMACROS

#include "windef.h"
#include "winbase.h"
#include "mfidl.h"
#include "rpcproxy.h"

#include "initguid.h"
#include "mf.h"

#undef INITGUID
#include <guiddef.h>
#include "mfapi.h"
#include "mferror.h"

#include "wine/debug.h"
#include "wine/heap.h"
#include "wine/unicode.h"
#include "wine/list.h"

WINE_DEFAULT_DEBUG_CHANNEL(mfplat);

static HINSTANCE mf_instance;

struct class_factory
{
    IClassFactory IClassFactory_iface;
    HRESULT (*create_instance)(REFIID riid, void **obj);
};

static inline struct class_factory *impl_from_IClassFactory(IClassFactory *iface)
{
    return CONTAINING_RECORD(iface, struct class_factory, IClassFactory_iface);
}

static HRESULT WINAPI class_factory_QueryInterface(IClassFactory *iface, REFIID riid, void **obj)
{
    TRACE("%p, %s, %p.\n", iface, debugstr_guid(riid), obj);

    if (IsEqualGUID(riid, &IID_IClassFactory) ||
            IsEqualGUID(riid, &IID_IUnknown))
    {
        *obj = iface;
        IClassFactory_AddRef(iface);
        return S_OK;
    }

    WARN("%s is not supported.\n", debugstr_guid(riid));
    *obj = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI class_factory_AddRef(IClassFactory *iface)
{
    return 2;
}

static ULONG WINAPI class_factory_Release(IClassFactory *iface)
{
    return 1;
}

static HRESULT WINAPI class_factory_CreateInstance(IClassFactory *iface, IUnknown *outer, REFIID riid, void **obj)
{
    struct class_factory *factory = impl_from_IClassFactory(iface);

    TRACE("%p, %p, %s, %p.\n", iface, outer, debugstr_guid(riid), obj);

    if (outer)
    {
        *obj = NULL;
        return CLASS_E_NOAGGREGATION;
    }

    return factory->create_instance(riid, obj);
}

static HRESULT WINAPI class_factory_LockServer(IClassFactory *iface, BOOL dolock)
{
    FIXME("%d.\n", dolock);

    return S_OK;
}

static const IClassFactoryVtbl class_factory_vtbl =
{
    class_factory_QueryInterface,
    class_factory_AddRef,
    class_factory_Release,
    class_factory_CreateInstance,
    class_factory_LockServer,
};

struct file_scheme_handler_result
{
    struct list entry;
    IMFAsyncResult *result;
    MF_OBJECT_TYPE obj_type;
    IUnknown *object;
};

struct file_scheme_handler
{
    IMFSchemeHandler IMFSchemeHandler_iface;
    IMFAsyncCallback IMFAsyncCallback_iface;
    LONG refcount;
    IMFSourceResolver *resolver;
    struct list results;
    CRITICAL_SECTION cs;
};

static struct file_scheme_handler *impl_from_IMFSchemeHandler(IMFSchemeHandler *iface)
{
    return CONTAINING_RECORD(iface, struct file_scheme_handler, IMFSchemeHandler_iface);
}

static struct file_scheme_handler *impl_from_IMFAsyncCallback(IMFAsyncCallback *iface)
{
    return CONTAINING_RECORD(iface, struct file_scheme_handler, IMFAsyncCallback_iface);
}

static HRESULT WINAPI file_scheme_handler_QueryInterface(IMFSchemeHandler *iface, REFIID riid, void **obj)
{
    TRACE("%p, %s, %p.\n", iface, debugstr_guid(riid), obj);

    if (IsEqualIID(riid, &IID_IMFSchemeHandler) ||
            IsEqualIID(riid, &IID_IUnknown))
    {
        *obj = iface;
        IMFSchemeHandler_AddRef(iface);
        return S_OK;
    }

    WARN("Unsupported %s.\n", debugstr_guid(riid));
    *obj = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI file_scheme_handler_AddRef(IMFSchemeHandler *iface)
{
    struct file_scheme_handler *handler = impl_from_IMFSchemeHandler(iface);
    ULONG refcount = InterlockedIncrement(&handler->refcount);

    TRACE("%p, refcount %u.\n", handler, refcount);

    return refcount;
}

static ULONG WINAPI file_scheme_handler_Release(IMFSchemeHandler *iface)
{
    struct file_scheme_handler *handler = impl_from_IMFSchemeHandler(iface);
    ULONG refcount = InterlockedDecrement(&handler->refcount);
    struct file_scheme_handler_result *result, *next;

    TRACE("%p, refcount %u.\n", iface, refcount);

    if (!refcount)
    {
        LIST_FOR_EACH_ENTRY_SAFE(result, next, &handler->results, struct file_scheme_handler_result, entry)
        {
            list_remove(&result->entry);
            IMFAsyncResult_Release(result->result);
            if (result->object)
                IUnknown_Release(result->object);
            heap_free(result);
        }
        DeleteCriticalSection(&handler->cs);
        if (handler->resolver)
            IMFSourceResolver_Release(handler->resolver);
        heap_free(handler);
    }

    return refcount;
}

struct create_object_context
{
    IUnknown IUnknown_iface;
    LONG refcount;

    IPropertyStore *props;
    WCHAR *url;
    DWORD flags;
};

static struct create_object_context *impl_from_IUnknown(IUnknown *iface)
{
    return CONTAINING_RECORD(iface, struct create_object_context, IUnknown_iface);
}

static HRESULT WINAPI create_object_context_QueryInterface(IUnknown *iface, REFIID riid, void **obj)
{
    TRACE("%p, %s, %p.\n", iface, debugstr_guid(riid), obj);

    if (IsEqualIID(riid, &IID_IUnknown))
    {
        *obj = iface;
        IUnknown_AddRef(iface);
        return S_OK;
    }

    WARN("Unsupported %s.\n", debugstr_guid(riid));
    *obj = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI create_object_context_AddRef(IUnknown *iface)
{
    struct create_object_context *context = impl_from_IUnknown(iface);
    ULONG refcount = InterlockedIncrement(&context->refcount);

    TRACE("%p, refcount %u.\n", iface, refcount);

    return refcount;
}

static ULONG WINAPI create_object_context_Release(IUnknown *iface)
{
    struct create_object_context *context = impl_from_IUnknown(iface);
    ULONG refcount = InterlockedDecrement(&context->refcount);

    TRACE("%p, refcount %u.\n", iface, refcount);

    if (!refcount)
    {
        if (context->props)
            IPropertyStore_Release(context->props);
        heap_free(context->url);
        heap_free(context);
    }

    return refcount;
}

static const IUnknownVtbl create_object_context_vtbl =
{
    create_object_context_QueryInterface,
    create_object_context_AddRef,
    create_object_context_Release,
};

static WCHAR *heap_strdupW(const WCHAR *str)
{
    WCHAR *ret = NULL;

    if (str)
    {
        unsigned int size;

        size = (strlenW(str) + 1) * sizeof(WCHAR);
        ret = heap_alloc(size);
        if (ret)
            memcpy(ret, str, size);
    }

    return ret;
}

static HRESULT WINAPI file_scheme_handler_BeginCreateObject(IMFSchemeHandler *iface, const WCHAR *url, DWORD flags,
        IPropertyStore *props, IUnknown **cancel_cookie, IMFAsyncCallback *callback, IUnknown *state)
{
    struct file_scheme_handler *handler = impl_from_IMFSchemeHandler(iface);
    struct create_object_context *context;
    IMFAsyncResult *caller, *item;
    HRESULT hr;

    TRACE("%p, %s, %#x, %p, %p, %p, %p.\n", iface, debugstr_w(url), flags, props, cancel_cookie, callback, state);

    if (cancel_cookie)
        *cancel_cookie = NULL;

    if (FAILED(hr = MFCreateAsyncResult(NULL, callback, state, &caller)))
        return hr;

    context = heap_alloc(sizeof(*context));
    if (!context)
    {
        IMFAsyncResult_Release(caller);
        return E_OUTOFMEMORY;
    }

    context->IUnknown_iface.lpVtbl = &create_object_context_vtbl;
    context->refcount = 1;
    context->props = props;
    if (context->props)
        IPropertyStore_AddRef(context->props);
    context->flags = flags;
    context->url = heap_strdupW(url);
    if (!context->url)
    {
        IMFAsyncResult_Release(caller);
        IUnknown_Release(&context->IUnknown_iface);
        return E_OUTOFMEMORY;
    }

    hr = MFCreateAsyncResult(&context->IUnknown_iface, &handler->IMFAsyncCallback_iface, (IUnknown *)caller, &item);
    IUnknown_Release(&context->IUnknown_iface);
    IMFAsyncResult_Release(caller);
    if (SUCCEEDED(hr))
    {
        if (SUCCEEDED(hr = MFPutWorkItemEx(MFASYNC_CALLBACK_QUEUE_IO, item)))
        {
            if (cancel_cookie)
                IMFAsyncResult_GetState(item, cancel_cookie);
        }

        IMFAsyncResult_Release(item);
    }

    return hr;
}

static HRESULT WINAPI file_scheme_handler_EndCreateObject(IMFSchemeHandler *iface, IMFAsyncResult *result,
        MF_OBJECT_TYPE *obj_type, IUnknown **object)
{
    struct file_scheme_handler *handler = impl_from_IMFSchemeHandler(iface);
    struct file_scheme_handler_result *found = NULL, *cur;
    HRESULT hr;

    TRACE("%p, %p, %p, %p.\n", iface, result, obj_type, object);

    EnterCriticalSection(&handler->cs);

    LIST_FOR_EACH_ENTRY(cur, &handler->results, struct file_scheme_handler_result, entry)
    {
        if (result == cur->result)
        {
            list_remove(&cur->entry);
            found = cur;
            break;
        }
    }

    LeaveCriticalSection(&handler->cs);

    if (found)
    {
        *obj_type = found->obj_type;
        *object = found->object;
        hr = IMFAsyncResult_GetStatus(found->result);
        IMFAsyncResult_Release(found->result);
        heap_free(found);
    }
    else
    {
        *obj_type = MF_OBJECT_INVALID;
        *object = NULL;
        hr = MF_E_UNEXPECTED;
    }

    return hr;
}

static HRESULT WINAPI file_scheme_handler_CancelObjectCreation(IMFSchemeHandler *iface, IUnknown *cancel_cookie)
{
    struct file_scheme_handler *handler = impl_from_IMFSchemeHandler(iface);
    struct file_scheme_handler_result *found = NULL, *cur;

    TRACE("%p, %p.\n", iface, cancel_cookie);

    EnterCriticalSection(&handler->cs);

    LIST_FOR_EACH_ENTRY(cur, &handler->results, struct file_scheme_handler_result, entry)
    {
        if (cancel_cookie == (IUnknown *)cur->result)
        {
            list_remove(&cur->entry);
            found = cur;
            break;
        }
    }

    LeaveCriticalSection(&handler->cs);

    if (found)
    {
        IMFAsyncResult_Release(found->result);
        if (found->object)
            IUnknown_Release(found->object);
        heap_free(found);
    }

    return found ? S_OK : MF_E_UNEXPECTED;
}

static const IMFSchemeHandlerVtbl file_scheme_handler_vtbl =
{
    file_scheme_handler_QueryInterface,
    file_scheme_handler_AddRef,
    file_scheme_handler_Release,
    file_scheme_handler_BeginCreateObject,
    file_scheme_handler_EndCreateObject,
    file_scheme_handler_CancelObjectCreation,
};

static HRESULT WINAPI file_scheme_handler_callback_QueryInterface(IMFAsyncCallback *iface, REFIID riid, void **obj)
{
    if (IsEqualIID(riid, &IID_IMFAsyncCallback) ||
            IsEqualIID(riid, &IID_IUnknown))
    {
        *obj = iface;
        IMFAsyncCallback_AddRef(iface);
        return S_OK;
    }

    WARN("Unsupported %s.\n", debugstr_guid(riid));
    *obj = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI file_scheme_handler_callback_AddRef(IMFAsyncCallback *iface)
{
    struct file_scheme_handler *handler = impl_from_IMFAsyncCallback(iface);
    return IMFSchemeHandler_AddRef(&handler->IMFSchemeHandler_iface);
}

static ULONG WINAPI file_scheme_handler_callback_Release(IMFAsyncCallback *iface)
{
    struct file_scheme_handler *handler = impl_from_IMFAsyncCallback(iface);
    return IMFSchemeHandler_Release(&handler->IMFSchemeHandler_iface);
}

static HRESULT WINAPI file_scheme_handler_callback_GetParameters(IMFAsyncCallback *iface, DWORD *flags, DWORD *queue)
{
    return E_NOTIMPL;
}

static HRESULT file_scheme_handler_get_resolver(struct file_scheme_handler *handler, IMFSourceResolver **resolver)
{
    HRESULT hr;

    if (!handler->resolver)
    {
        IMFSourceResolver *resolver;

        if (FAILED(hr = MFCreateSourceResolver(&resolver)))
            return hr;

        if (InterlockedCompareExchangePointer((void **)&handler->resolver, resolver, NULL))
            IMFSourceResolver_Release(resolver);
    }

    *resolver = handler->resolver;
    IMFSourceResolver_AddRef(*resolver);

    return S_OK;
}

static HRESULT WINAPI file_scheme_handler_callback_Invoke(IMFAsyncCallback *iface, IMFAsyncResult *result)
{
    static const WCHAR schemeW[] = {'f','i','l','e',':','/','/'};
    struct file_scheme_handler *handler = impl_from_IMFAsyncCallback(iface);
    struct file_scheme_handler_result *handler_result;
    MF_OBJECT_TYPE obj_type = MF_OBJECT_INVALID;
    IUnknown *object = NULL, *context_object;
    struct create_object_context *context;
    IMFSourceResolver *resolver;
    IMFAsyncResult *caller;
    IMFByteStream *stream;
    const WCHAR *url;
    HRESULT hr;

    caller = (IMFAsyncResult *)IMFAsyncResult_GetStateNoAddRef(result);

    if (FAILED(hr = IMFAsyncResult_GetObject(result, &context_object)))
    {
        WARN("Expected context set for callee result.\n");
        return hr;
    }

    context = impl_from_IUnknown(context_object);

    /* Strip from scheme, MFCreateFile() won't be expecting it. */
    url = context->url;
    if (!strncmpiW(context->url, schemeW, ARRAY_SIZE(schemeW)))
        url += ARRAY_SIZE(schemeW);

    hr = MFCreateFile(context->flags & MF_RESOLUTION_WRITE ? MF_ACCESSMODE_READWRITE : MF_ACCESSMODE_READ,
            MF_OPENMODE_FAIL_IF_NOT_EXIST, MF_FILEFLAGS_NONE, url, &stream);
    if (SUCCEEDED(hr))
    {
        if (context->flags & MF_RESOLUTION_MEDIASOURCE)
        {
            if (SUCCEEDED(hr = file_scheme_handler_get_resolver(handler, &resolver)))
            {
                hr = IMFSourceResolver_CreateObjectFromByteStream(resolver, stream, context->url, context->flags,
                        context->props, &obj_type, &object);
                IMFSourceResolver_Release(resolver);
                IMFByteStream_Release(stream);
            }
        }
        else
        {
            object = (IUnknown *)stream;
            obj_type = MF_OBJECT_BYTESTREAM;
        }
    }

    handler_result = heap_alloc(sizeof(*handler_result));
    if (handler_result)
    {
        handler_result->result = caller;
        IMFAsyncResult_AddRef(handler_result->result);
        handler_result->obj_type = obj_type;
        handler_result->object = object;

        EnterCriticalSection(&handler->cs);
        list_add_tail(&handler->results, &handler_result->entry);
        LeaveCriticalSection(&handler->cs);
    }
    else
    {
        if (object)
            IUnknown_Release(object);
        hr = E_OUTOFMEMORY;
    }

    IUnknown_Release(&context->IUnknown_iface);

    IMFAsyncResult_SetStatus(caller, hr);
    MFInvokeCallback(caller);

    return S_OK;
}

static const IMFAsyncCallbackVtbl file_scheme_handler_callback_vtbl =
{
    file_scheme_handler_callback_QueryInterface,
    file_scheme_handler_callback_AddRef,
    file_scheme_handler_callback_Release,
    file_scheme_handler_callback_GetParameters,
    file_scheme_handler_callback_Invoke,
};

static HRESULT file_scheme_handler_construct(REFIID riid, void **obj)
{
    struct file_scheme_handler *handler;
    HRESULT hr;

    TRACE("%s, %p.\n", debugstr_guid(riid), obj);

    handler = heap_alloc_zero(sizeof(*handler));
    if (!handler)
        return E_OUTOFMEMORY;

    handler->IMFSchemeHandler_iface.lpVtbl = &file_scheme_handler_vtbl;
    handler->IMFAsyncCallback_iface.lpVtbl = &file_scheme_handler_callback_vtbl;
    handler->refcount = 1;
    list_init(&handler->results);
    InitializeCriticalSection(&handler->cs);

    hr = IMFSchemeHandler_QueryInterface(&handler->IMFSchemeHandler_iface, riid, obj);
    IMFSchemeHandler_Release(&handler->IMFSchemeHandler_iface);

    return hr;
}

static struct class_factory file_scheme_handler_factory = { { &class_factory_vtbl }, file_scheme_handler_construct };

static const struct class_object
{
    const GUID *clsid;
    IClassFactory *factory;
}
class_objects[] =
{
    { &CLSID_FileSchemeHandler, &file_scheme_handler_factory.IClassFactory_iface },
};

/*******************************************************************************
 *      DllGetClassObject (mf.@)
 */
HRESULT WINAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void **obj)
{
    unsigned int i;

    TRACE("%s, %s, %p.\n", debugstr_guid(rclsid), debugstr_guid(riid), obj);

    for (i = 0; i < ARRAY_SIZE(class_objects); ++i)
    {
        if (IsEqualGUID(class_objects[i].clsid, rclsid))
            return IClassFactory_QueryInterface(class_objects[i].factory, riid, obj);
    }

    WARN("%s: class not found.\n", debugstr_guid(rclsid));
    return CLASS_E_CLASSNOTAVAILABLE;
}

/******************************************************************
 *              DllCanUnloadNow (mf.@)
 */
HRESULT WINAPI DllCanUnloadNow(void)
{
    return S_FALSE;
}

/***********************************************************************
 *          DllRegisterServer (mf.@)
 */
HRESULT WINAPI DllRegisterServer(void)
{
    return __wine_register_resources( mf_instance );
}

/***********************************************************************
 *          DllUnregisterServer (mf.@)
 */
HRESULT WINAPI DllUnregisterServer(void)
{
    return __wine_unregister_resources( mf_instance );
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    switch (reason)
    {
        case DLL_WINE_PREATTACH:
            return FALSE;    /* prefer native version */
        case DLL_PROCESS_ATTACH:
            mf_instance = instance;
            DisableThreadLibraryCalls(instance);
            break;
    }

    return TRUE;
}

/***********************************************************************
 *      MFGetSupportedMimeTypes (mf.@)
 */
HRESULT WINAPI MFGetSupportedMimeTypes(PROPVARIANT *array)
{
    FIXME("(%p) stub\n", array);

    return E_NOTIMPL;
}

/***********************************************************************
 *      MFGetService (mf.@)
 */
HRESULT WINAPI MFGetService(IUnknown *object, REFGUID service, REFIID riid, void **obj)
{
    IMFGetService *gs;
    HRESULT hr;

    TRACE("(%p, %s, %s, %p)\n", object, debugstr_guid(service), debugstr_guid(riid), obj);

    if (!object)
        return E_POINTER;

    if (FAILED(hr = IUnknown_QueryInterface(object, &IID_IMFGetService, (void **)&gs)))
        return hr;

    hr = IMFGetService_GetService(gs, service, riid, obj);
    IMFGetService_Release(gs);
    return hr;
}

/***********************************************************************
 *      MFShutdownObject (mf.@)
 */
HRESULT WINAPI MFShutdownObject(IUnknown *object)
{
    IMFShutdown *shutdown;

    TRACE("%p.\n", object);

    if (object && SUCCEEDED(IUnknown_QueryInterface(object, &IID_IMFShutdown, (void **)&shutdown)))
    {
        IMFShutdown_Shutdown(shutdown);
        IMFShutdown_Release(shutdown);
    }

    return S_OK;
}

/***********************************************************************
 *      MFEnumDeviceSources (mf.@)
 */
HRESULT WINAPI MFEnumDeviceSources(IMFAttributes *attributes, IMFActivate ***sources, UINT32 *count)
{
    FIXME("%p, %p, %p.\n", attributes, sources, count);

    if (!attributes || !sources || !count)
        return E_INVALIDARG;

    *count = 0;

    return S_OK;
}
