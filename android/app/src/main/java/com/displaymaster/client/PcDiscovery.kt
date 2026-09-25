package com.displaymaster.client

import android.content.Context
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import android.os.Build
import android.util.Log
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import java.util.concurrent.Executors

data class DiscoveredPc(val serviceName: String, val name: String, val address: String, val port: Int)

/**
 * Finds PCs running DisplayMaster with Wi-Fi enabled: they advertise `_displaymaster._tcp`
 * over mDNS (see windows/host/src/transport/mdns.cpp).
 */
class PcDiscovery(context: Context) {
    private val nsd = context.getSystemService(Context.NSD_SERVICE) as NsdManager
    private val executor = Executors.newSingleThreadExecutor()
    private val _pcs = MutableStateFlow<List<DiscoveredPc>>(emptyList())
    val pcs: StateFlow<List<DiscoveredPc>> = _pcs.asStateFlow()

    private var listener: NsdManager.DiscoveryListener? = null
    private val infoCallbacks = mutableMapOf<String, NsdManager.ServiceInfoCallback>()

    // Pre-Android 14 resolveService() handles one request at a time.
    private val resolveQueue = ArrayDeque<NsdServiceInfo>()
    private var resolving = false

    fun start() {
        if (listener != null) return
        val l = object : NsdManager.DiscoveryListener {
            override fun onDiscoveryStarted(type: String) = Unit
            override fun onDiscoveryStopped(type: String) = Unit
            override fun onStartDiscoveryFailed(type: String, error: Int) {
                Log.w(TAG, "discovery failed: $error")
                listener = null
            }
            override fun onStopDiscoveryFailed(type: String, error: Int) = Unit
            override fun onServiceFound(info: NsdServiceInfo) = resolve(info)
            override fun onServiceLost(info: NsdServiceInfo) {
                _pcs.update { list -> list.filterNot { it.serviceName == info.serviceName } }
                if (Build.VERSION.SDK_INT >= 34) {
                    infoCallbacks.remove(info.serviceName)?.let { runCatching { nsd.unregisterServiceInfoCallback(it) } }
                }
            }
        }
        listener = l
        nsd.discoverServices(SERVICE_TYPE, NsdManager.PROTOCOL_DNS_SD, l)
    }

    fun stop() {
        listener?.let { runCatching { nsd.stopServiceDiscovery(it) } }
        listener = null
        if (Build.VERSION.SDK_INT >= 34) {
            infoCallbacks.values.forEach { runCatching { nsd.unregisterServiceInfoCallback(it) } }
        }
        infoCallbacks.clear()
        synchronized(resolveQueue) { resolveQueue.clear() }
        _pcs.value = emptyList()
    }

    private fun resolve(info: NsdServiceInfo) {
        if (Build.VERSION.SDK_INT >= 34) {
            val cb = object : NsdManager.ServiceInfoCallback {
                override fun onServiceInfoCallbackRegistrationFailed(error: Int) = Unit
                override fun onServiceUpdated(resolved: NsdServiceInfo) = publish(resolved)
                override fun onServiceLost() {
                    _pcs.update { list -> list.filterNot { it.serviceName == info.serviceName } }
                }
                override fun onServiceInfoCallbackUnregistered() = Unit
            }
            infoCallbacks[info.serviceName] = cb
            nsd.registerServiceInfoCallback(info, executor, cb)
        } else {
            synchronized(resolveQueue) { resolveQueue.addLast(info) }
            resolveNext()
        }
    }

    @Suppress("DEPRECATION")
    private fun resolveNext() {
        val next = synchronized(resolveQueue) {
            if (resolving || resolveQueue.isEmpty()) return
            resolving = true
            resolveQueue.removeFirst()
        }
        nsd.resolveService(next, object : NsdManager.ResolveListener {
            override fun onResolveFailed(info: NsdServiceInfo, error: Int) = done()
            override fun onServiceResolved(info: NsdServiceInfo) {
                publish(info)
                done()
            }
            private fun done() {
                synchronized(resolveQueue) { resolving = false }
                resolveNext()
            }
        })
    }

    private fun publish(info: NsdServiceInfo) {
        val host = if (Build.VERSION.SDK_INT >= 34) {
            info.hostAddresses.firstOrNull { it is java.net.Inet4Address } ?: info.hostAddresses.firstOrNull()
        } else {
            @Suppress("DEPRECATION") info.host
        } ?: return
        val friendly = info.attributes["name"]?.toString(Charsets.UTF_8) ?: info.serviceName
        val pc = DiscoveredPc(info.serviceName, friendly, host.hostAddress ?: return, info.port)
        _pcs.update { list -> (list.filterNot { it.serviceName == pc.serviceName } + pc).sortedBy { it.name } }
    }

    companion object {
        private const val TAG = "PcDiscovery"
        const val SERVICE_TYPE = "_displaymaster._tcp"
    }
}
