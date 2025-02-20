/**
 * OpenAL cross platform audio library
 * Copyright (C) 2010 by Chris Robinson
 * This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 *  License along with this library; if not, write to the
 *  Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 * Or go to http://www.gnu.org/copyleft/lgpl.html
 */

#include "config.h"

#include "pipewire.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cerrno>
#include <chrono>
#include <ctime>
#include <list>
#include <memory>
#include <mutex>
#include <stdint.h>
#include <utility>

#include "albyte.h"
#include "almalloc.h"
#include "alnumeric.h"
#include "aloptional.h"
#include "alspan.h"
#include "alstring.h"
#include "core/devformat.h"
#include "core/device.h"
#include "core/helpers.h"
#include "core/logging.h"
#include "dynload.h"
#include "opthelpers.h"
#include "ringbuffer.h"

/* Ignore warnings caused by PipeWire headers (lots in standard C++ mode). */
_Pragma("GCC diagnostic push")
_Pragma("GCC diagnostic ignored \"-Weverything\"")
#include "pipewire/pipewire.h"
#include "pipewire/extensions/metadata.h"
#include "spa/buffer/buffer.h"
#include "spa/param/audio/format-utils.h"
#include "spa/param/audio/raw.h"
#include "spa/param/param.h"
#include "spa/pod/builder.h"
#include "spa/utils/json.h"

namespace {
/* Wrap some nasty macros here too... */
template<typename ...Args>
auto ppw_core_add_listener(pw_core *core, Args&& ...args)
{ return pw_core_add_listener(core, std::forward<Args>(args)...); }
template<typename ...Args>
auto ppw_core_sync(pw_core *core, Args&& ...args)
{ return pw_core_sync(core, std::forward<Args>(args)...); }
template<typename ...Args>
auto ppw_node_subscribe_params(pw_proxy *proxy, Args&& ...args)
{ return pw_node_subscribe_params(proxy, std::forward<Args>(args)...); }
template<typename ...Args>
auto ppw_registry_add_listener(pw_registry *reg, Args&& ...args)
{ return pw_registry_add_listener(reg, std::forward<Args>(args)...); }


constexpr auto get_pod_type(const spa_pod *pod) noexcept
{ return SPA_POD_TYPE(pod); }

template<typename T>
constexpr auto get_pod_body(const spa_pod *pod) noexcept
{ return static_cast<T*>(SPA_POD_BODY(pod)); }

constexpr auto make_pod_builder(void *data, uint32_t size) noexcept
{ return SPA_POD_BUILDER_INIT(data, size); }

constexpr auto PwIdAny = PW_ID_ANY;

} // namespace
_Pragma("GCC diagnostic pop")

namespace {

using std::chrono::seconds;
using std::chrono::nanoseconds;
using uint = unsigned int;

constexpr char pwireDevice[] = "PipeWire Output";
constexpr char pwireInput[] = "PipeWire Input";


#ifdef HAVE_DYNLOAD
#define PWIRE_FUNCS(MAGIC)                                                    \
    MAGIC(pw_context_connect)                                                 \
    MAGIC(pw_context_destroy)                                                 \
    MAGIC(pw_context_new)                                                     \
    MAGIC(pw_core_disconnect)                                                 \
    MAGIC(pw_init)                                                            \
    MAGIC(pw_properties_free)                                                 \
    MAGIC(pw_properties_new)                                                  \
    MAGIC(pw_properties_set)                                                  \
    MAGIC(pw_properties_setf)                                                 \
    MAGIC(pw_proxy_add_object_listener)                                       \
    MAGIC(pw_proxy_destroy)                                                   \
    MAGIC(pw_proxy_get_user_data)                                             \
    MAGIC(pw_stream_connect)                                                  \
    MAGIC(pw_stream_dequeue_buffer)                                           \
    MAGIC(pw_stream_destroy)                                                  \
    MAGIC(pw_stream_get_state)                                                \
    MAGIC(pw_stream_get_time)                                                 \
    MAGIC(pw_stream_new_simple)                                               \
    MAGIC(pw_stream_queue_buffer)                                             \
    MAGIC(pw_stream_set_active)                                               \
    MAGIC(pw_thread_loop_new)                                                 \
    MAGIC(pw_thread_loop_destroy)                                             \
    MAGIC(pw_thread_loop_get_loop)                                            \
    MAGIC(pw_thread_loop_start)                                               \
    MAGIC(pw_thread_loop_stop)                                                \
    MAGIC(pw_thread_loop_lock)                                                \
    MAGIC(pw_thread_loop_wait)                                                \
    MAGIC(pw_thread_loop_signal)                                              \
    MAGIC(pw_thread_loop_unlock)                                              \

void *pwire_handle;
#define MAKE_FUNC(f) decltype(f) * p##f;
PWIRE_FUNCS(MAKE_FUNC)
#undef MAKE_FUNC

bool pwire_load()
{
    if(pwire_handle)
        return true;

    static constexpr char pwire_library[] = "libpipewire-0.3.so.0";
    std::string missing_funcs;

    pwire_handle = LoadLib(pwire_library);
    if(!pwire_handle)
    {
        WARN("Failed to load %s\n", pwire_library);
        return false;
    }

#define LOAD_FUNC(f) do {                                                     \
    p##f = reinterpret_cast<decltype(p##f)>(GetSymbol(pwire_handle, #f));     \
    if(p##f == nullptr) missing_funcs += "\n" #f;                             \
} while(0);
    PWIRE_FUNCS(LOAD_FUNC)
#undef LOAD_FUNC

    if(!missing_funcs.empty())
    {
        WARN("Missing expected functions:%s\n", missing_funcs.c_str());
        CloseLib(pwire_handle);
        pwire_handle = nullptr;
        return false;
    }

    return true;
}

#ifndef IN_IDE_PARSER
#define pw_context_connect ppw_context_connect
#define pw_context_destroy ppw_context_destroy
#define pw_context_new ppw_context_new
#define pw_core_disconnect ppw_core_disconnect
#define pw_init ppw_init
#define pw_properties_free ppw_properties_free
#define pw_properties_new ppw_properties_new
#define pw_properties_set ppw_properties_set
#define pw_properties_setf ppw_properties_setf
#define pw_proxy_add_object_listener ppw_proxy_add_object_listener
#define pw_proxy_destroy ppw_proxy_destroy
#define pw_proxy_get_user_data ppw_proxy_get_user_data
#define pw_stream_connect ppw_stream_connect
#define pw_stream_dequeue_buffer ppw_stream_dequeue_buffer
#define pw_stream_destroy ppw_stream_destroy
#define pw_stream_get_state ppw_stream_get_state
#define pw_stream_get_time ppw_stream_get_time
#define pw_stream_new_simple ppw_stream_new_simple
#define pw_stream_queue_buffer ppw_stream_queue_buffer
#define pw_stream_set_active ppw_stream_set_active
#define pw_thread_loop_destroy ppw_thread_loop_destroy
#define pw_thread_loop_get_loop ppw_thread_loop_get_loop
#define pw_thread_loop_lock ppw_thread_loop_lock
#define pw_thread_loop_new ppw_thread_loop_new
#define pw_thread_loop_signal ppw_thread_loop_signal
#define pw_thread_loop_start ppw_thread_loop_start
#define pw_thread_loop_stop ppw_thread_loop_stop
#define pw_thread_loop_unlock ppw_thread_loop_unlock
#define pw_thread_loop_wait ppw_thread_loop_wait
#endif

#else

constexpr bool pwire_load() { return true; }
#endif


class ThreadMainloop {
    pw_thread_loop *mLoop{};

public:
    ThreadMainloop() = default;
    ThreadMainloop(const ThreadMainloop&) = delete;
    ThreadMainloop(ThreadMainloop&& rhs) noexcept : mLoop{rhs.mLoop} { rhs.mLoop = nullptr; }
    explicit ThreadMainloop(pw_thread_loop *loop) noexcept : mLoop{loop} { }
    ~ThreadMainloop() { if(mLoop) pw_thread_loop_destroy(mLoop); }

    ThreadMainloop& operator=(const ThreadMainloop&) = delete;
    ThreadMainloop& operator=(ThreadMainloop&& rhs) noexcept
    { std::swap(mLoop, rhs.mLoop); return *this; }

    operator bool() const noexcept { return mLoop != nullptr; }

    auto start() const { return pw_thread_loop_start(mLoop); }
    auto stop() const { return pw_thread_loop_stop(mLoop); }

    auto signal(bool wait) const { return pw_thread_loop_signal(mLoop, wait); }
    auto wait() const { return pw_thread_loop_wait(mLoop); }

    auto getLoop() const { return pw_thread_loop_get_loop(mLoop); }

    auto lock() const { return pw_thread_loop_lock(mLoop); }
    auto unlock() const { return pw_thread_loop_unlock(mLoop); }
};
using MainloopUniqueLock = std::unique_lock<ThreadMainloop>;
using MainloopLockGuard = std::lock_guard<ThreadMainloop>;

struct PwStreamDeleter {
    void operator()(pw_stream *stream) const { pw_stream_destroy(stream); }
};
using PwStreamPtr = std::unique_ptr<pw_stream,PwStreamDeleter>;


/* Enums for bitflags... again... *sigh* */
constexpr pw_stream_flags operator|(pw_stream_flags lhs, pw_stream_flags rhs) noexcept
{ return static_cast<pw_stream_flags>(lhs | uint{rhs}); }


const spa_audio_channel MonoMap[]{
    SPA_AUDIO_CHANNEL_MONO
}, StereoMap[] {
    SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR
}, QuadMap[]{
    SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR, SPA_AUDIO_CHANNEL_RL, SPA_AUDIO_CHANNEL_RR
}, X51Map[]{
    SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR, SPA_AUDIO_CHANNEL_FC, SPA_AUDIO_CHANNEL_LFE,
    SPA_AUDIO_CHANNEL_SL, SPA_AUDIO_CHANNEL_SR
}, X51RearMap[]{
    SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR, SPA_AUDIO_CHANNEL_FC, SPA_AUDIO_CHANNEL_LFE,
    SPA_AUDIO_CHANNEL_RL, SPA_AUDIO_CHANNEL_RR
}, X61Map[]{
    SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR, SPA_AUDIO_CHANNEL_FC, SPA_AUDIO_CHANNEL_LFE,
    SPA_AUDIO_CHANNEL_RC, SPA_AUDIO_CHANNEL_SL, SPA_AUDIO_CHANNEL_SR
}, X71Map[]{
    SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR, SPA_AUDIO_CHANNEL_FC, SPA_AUDIO_CHANNEL_LFE,
    SPA_AUDIO_CHANNEL_RL, SPA_AUDIO_CHANNEL_RR, SPA_AUDIO_CHANNEL_SL, SPA_AUDIO_CHANNEL_SR
};

/**
 * Checks if every channel in 'map1' exists in 'map0' (that is, map0 is equal
 * to or a superset of map1).
 */
template<size_t N>
bool MatchChannelMap(const al::span<uint32_t> map0, const spa_audio_channel (&map1)[N])
{
    for(const spa_audio_channel chid : map1)
    {
        if(std::find(map0.begin(), map0.end(), chid) == map0.end())
            return false;
    }
    return true;
}


/* There's quite a mess here, but the purpose is to track active devices and
 * their default formats, so playback devices can be configured to match. The
 * device list is updated asynchronously, so it will have the latest list of
 * devices provided by the server.
 */

struct NodeProxy;
struct MetadataProxy;

/* The global thread watching for global events. This particular class responds
 * to objects being added to or removed from the registry.
 */
struct EventManager {
    ThreadMainloop mLoop{};
    pw_core *mCore{};
    pw_context *mContext{};
    pw_registry *mRegistry{};
    spa_hook mRegistryListener{};
    spa_hook mCoreListener{};

    /* A list of proxy objects watching for events about changes to objects in
     * the registry.
     */
    std::vector<NodeProxy*> mProxyList;
    MetadataProxy *mDefaultMetadata{nullptr};

    /* Initialization handling. When init() is called, mInitSeq is set to a
     * SequenceID that marks the end of populating the registry. As objects of
     * interest are found, events to parse them are generated and mInitSeq is
     * updated with a newer ID. When mInitSeq stops being updated and the event
     * corresponding to it is reached, mInitDone will be set to true.
     */
    std::atomic<bool> mInitDone{false};
    int mInitSeq{};

    bool init();
    ~EventManager();

    auto lock() const { return mLoop.lock(); }
    auto unlock() const { return mLoop.unlock(); }

    /**
     * Waits for initialization to finish. The event manager must be locked
     * when calling this.
     */
    void waitForInit()
    {
        while(unlikely(!mInitDone.load(std::memory_order_acquire)))
            mLoop.wait();
    }

    void syncInit()
    {
        /* If initialization isn't done, update the sequence ID so it won't
         * complete until after currently scheduled events.
         */
        if(!mInitDone.load(std::memory_order_relaxed))
            mInitSeq = ppw_core_sync(mCore, PW_ID_CORE, mInitSeq);
    }

    void addCallback(uint32_t id, uint32_t permissions, const char *type, uint32_t version,
        const spa_dict *props);
    static void addCallbackC(void *object, uint32_t id, uint32_t permissions, const char *type,
        uint32_t version, const spa_dict *props)
    { static_cast<EventManager*>(object)->addCallback(id, permissions, type, version, props); }

    void removeCallback(uint32_t id);
    static void removeCallbackC(void *object, uint32_t id)
    { static_cast<EventManager*>(object)->removeCallback(id); }

    static const pw_registry_events sRegistryEvents;
    static constexpr pw_registry_events CreateRegistryEvents()
    {
        pw_registry_events ret{};
        ret.version = PW_VERSION_REGISTRY_EVENTS;
        ret.global = &EventManager::addCallbackC;
        ret.global_remove = &EventManager::removeCallbackC;
        return ret;
    }

    void coreCallback(uint32_t id, int seq);
    static void coreCallbackC(void *object, uint32_t id, int seq)
    { static_cast<EventManager*>(object)->coreCallback(id, seq); }

    static const pw_core_events sCoreEvents;
    static constexpr pw_core_events CreateCoreEvents()
    {
        pw_core_events ret{};
        ret.version = PW_VERSION_CORE_EVENTS;
        ret.done = &EventManager::coreCallbackC;
        return ret;
    }
};
using EventWatcherUniqueLock = std::unique_lock<EventManager>;
using EventWatcherLockGuard = std::lock_guard<EventManager>;

const pw_core_events EventManager::sCoreEvents{EventManager::CreateCoreEvents()};
const pw_registry_events EventManager::sRegistryEvents{EventManager::CreateRegistryEvents()};
EventManager gEventHandler;

/* Enumerated devices. This is updated asynchronously as the app runs, and the
 * gEventHandler thread loop must be locked when accessing the list.
 */
constexpr auto InvalidChannelConfig = DevFmtChannels(255);
struct DeviceNode {
    std::string mName;
    std::string mDevName;

    uint32_t mId{};
    bool mCapture{};
    bool mIsHeadphones{};

    uint mSampleRate{};
    DevFmtChannels mChannels{InvalidChannelConfig};
};
constexpr char MonitorPrefix[]{"Monitor of "};
constexpr auto MonitorPrefixLen = al::size(MonitorPrefix) - 1;
constexpr char AudioSinkClass[]{"Audio/Sink"};
constexpr char AudioSourceClass[]{"Audio/Source"};
std::vector<DeviceNode> DeviceList;
std::string DefaultSinkDev;
std::string DefaultSourceDev;

DeviceNode &AddDeviceNode(uint32_t id)
{
    auto match_id = [id](DeviceNode &n) noexcept -> bool
    { return n.mId == id; };

    /* If the node is already in the list, return the existing entry. */
    auto match = std::find_if(DeviceList.begin(), DeviceList.end(), match_id);
    if(match != DeviceList.end()) return *match;

    DeviceList.emplace_back();
    auto &n = DeviceList.back();
    n.mId = id;
    return n;
}

DeviceNode *FindDeviceNode(uint32_t id)
{
    auto match_id = [id](DeviceNode &n) noexcept -> bool
    { return n.mId == id; };

    auto match = std::find_if(DeviceList.begin(), DeviceList.end(), match_id);
    if(match != DeviceList.end()) return std::addressof(*match);

    return nullptr;
}

void RemoveDevice(uint32_t id)
{
    auto match_id = [id](DeviceNode &n) noexcept -> bool
    { return n.mId == id; };

    auto end = std::remove_if(DeviceList.begin(), DeviceList.end(), match_id);
    DeviceList.erase(end, DeviceList.end());
}


/* A generic PipeWire node proxy object used to track changes to sink and
 * source nodes.
 */
struct NodeProxy {
    uint32_t mId{};

    pw_proxy *mProxy{nullptr};
    spa_hook mNodeListener{};

    NodeProxy(uint32_t id, pw_proxy *proxy)
      : mId{id}, mProxy{proxy}
    {
        pw_proxy_add_object_listener(mProxy, &mNodeListener, &sNodeEvents, this);

        /* Track changes to the enumerable formats (indicates the default
         * format, which is what we're interested in).
         */
        uint32_t fmtids[]{SPA_PARAM_EnumFormat};
        ppw_node_subscribe_params(mProxy, al::data(fmtids), al::size(fmtids));
    }
    ~NodeProxy()
    {
        spa_hook_remove(&mNodeListener);
        pw_proxy_destroy(mProxy);
    }


    void infoCallback(const pw_node_info *info);
    static void infoCallbackC(void *object, const pw_node_info *info)
    { static_cast<NodeProxy*>(object)->infoCallback(info); }

    void paramCallback(int seq, uint32_t id, uint32_t index, uint32_t next, const spa_pod *param);
    static void paramCallbackC(void *object, int seq, uint32_t id, uint32_t index, uint32_t next,
        const spa_pod *param)
    { static_cast<NodeProxy*>(object)->paramCallback(seq, id, index, next, param); }

    static const pw_node_events sNodeEvents;
    static constexpr pw_node_events CreateNodeEvents()
    {
        pw_node_events ret{};
        ret.version = PW_VERSION_NODE_EVENTS;
        ret.info = &NodeProxy::infoCallbackC;
        ret.param = &NodeProxy::paramCallbackC;
        return ret;
    }
};
const pw_node_events NodeProxy::sNodeEvents{NodeProxy::CreateNodeEvents()};

void NodeProxy::infoCallback(const pw_node_info *info)
{
    /* We only care about property changes here (media class, name/desc).
     * Format changes will automatically invoke the param callback.
     *
     * TODO: Can the media class or name/desc change without being removed and
     * readded?
     */
    if((info->change_mask&PW_NODE_CHANGE_MASK_PROPS))
    {
        /* Can this actually change? */
        const char *media_class{spa_dict_lookup(info->props, PW_KEY_MEDIA_CLASS)};
        if(unlikely(!media_class)) return;

        bool isCapture{};
        if(al::strcasecmp(media_class, AudioSinkClass) == 0)
            isCapture = false;
        else if(al::strcasecmp(media_class, AudioSourceClass) == 0)
            isCapture = true;
        else
        {
            TRACE("Dropping device node %u which became type \"%s\"\n", info->id, media_class);
            RemoveDevice(info->id);
            return;
        }

        bool isHeadphones{};
        if(const char *form_factor{spa_dict_lookup(info->props, PW_KEY_DEVICE_FORM_FACTOR)})
        {
            if(al::strcasecmp(form_factor, "headphones") == 0
                || al::strcasecmp(form_factor, "headset") == 0)
                isHeadphones = true;
        }

        const char *devName{spa_dict_lookup(info->props, PW_KEY_NODE_NAME)};
        const char *nodeName{spa_dict_lookup(info->props, PW_KEY_NODE_DESCRIPTION)};
        if(!nodeName || !*nodeName) nodeName = spa_dict_lookup(info->props, PW_KEY_NODE_NICK);
        if(!nodeName || !*nodeName) nodeName = devName;

        TRACE("Got %s device \"%s\"%s\n", isCapture ? "capture" : "playback",
            devName ? devName : "(nil)", isHeadphones ? " (headphones)" : "");
        TRACE("  \"%s\" = ID %u\n", nodeName ? nodeName : "(nil)", info->id);

        DeviceNode &node = AddDeviceNode(info->id);
        if(nodeName && *nodeName) node.mName = nodeName;
        else node.mName = "PipeWire node #"+std::to_string(info->id);
        node.mDevName = devName ? devName : "";
        node.mCapture = isCapture;
        node.mIsHeadphones = isHeadphones;
    }
}

/* Helpers for retrieving values from params */
template<uint32_t T> struct PodInfo { };

template<>
struct PodInfo<SPA_TYPE_Int> {
    using Type = int32_t;
    static auto get_value(const spa_pod *pod, int32_t *val)
    { return spa_pod_get_int(pod, val); }
};
template<>
struct PodInfo<SPA_TYPE_Id> {
    using Type = uint32_t;
    static auto get_value(const spa_pod *pod, uint32_t *val)
    { return spa_pod_get_id(pod, val); }
};

template<uint32_t T>
using Pod_t = typename PodInfo<T>::Type;

template<uint32_t T, size_t N>
uint32_t get_param_array(const spa_pod *value, const al::span<Pod_t<T>,N> vals)
{
    return spa_pod_copy_array(value, T, vals.data(), static_cast<uint32_t>(vals.size()));
}

template<uint32_t T>
al::optional<Pod_t<T>> get_param(const spa_pod *value)
{
    Pod_t<T> val{};
    if(PodInfo<T>::get_value(value, &val) == 0)
        return val;
    return al::nullopt;
}

void parse_srate(DeviceNode *node, const spa_pod *value)
{
    /* TODO: Can this be anything else? Long, Float, Double? */
    uint32_t nvals{}, choiceType{};
    value = spa_pod_get_values(value, &nvals, &choiceType);

    const uint podType{get_pod_type(value)};
    if(podType != SPA_TYPE_Int)
    {
        WARN("Unhandled sample rate POD type: %u\n", podType);
        return;
    }

    if(choiceType == SPA_CHOICE_Range)
    {
        if(nvals != 3)
        {
            WARN("Unexpected SPA_CHOICE_Range count: %u\n", nvals);
            return;
        }
        std::array<int32_t,3> srates{};
        std::copy_n(get_pod_body<int32_t>(value), srates.size(), srates.begin());

        /* [0] is the default, [1] is the min, and [2] is the max. */
        TRACE("Device ID %u sample rate: %d (range: %d -> %d)\n", node->mId, srates[0], srates[1],
            srates[2]);
        srates[0] = clampi(srates[0], MIN_OUTPUT_RATE, MAX_OUTPUT_RATE);
        node->mSampleRate = static_cast<uint>(srates[0]);
        return;
    }

    if(choiceType == SPA_CHOICE_Enum)
    {
        if(nvals == 0)
        {
            WARN("Unexpected SPA_CHOICE_Enum count: %u\n", nvals);
            return;
        }

        auto srates = std::vector<int32_t>(nvals);
        std::copy_n(get_pod_body<int32_t>(value), srates.size(), srates.begin());

        /* [0] is the default, [1...size()-1] are available selections. */
        std::string others{(srates.size() > 1) ? std::to_string(srates[1]) : std::string{}};
        for(size_t i{2};i < srates.size();++i)
        {
            others += ", ";
            others += std::to_string(srates[i]);
        }
        TRACE("Device ID %u sample rate: %d (%s)\n", node->mId, srates[0], others.c_str());
        /* Pick the first rate listed that's within the allowed range (default
         * rate if possible).
         */
        for(const auto &rate : srates)
        {
            if(rate >= MIN_OUTPUT_RATE && rate <= MAX_OUTPUT_RATE)
            {
                node->mSampleRate = static_cast<uint>(rate);
                break;
            }
        }
        return;
    }

    if(choiceType == SPA_CHOICE_None)
    {
        if(nvals != 1)
        {
            WARN("Unexpected SPA_CHOICE_None count: %u\n", nvals);
            return;
        }

        int32_t srate{*get_pod_body<int32_t>(value)};

        TRACE("Device ID %u sample rate: %d\n", node->mId, srate);
        srate = clampi(srate, MIN_OUTPUT_RATE, MAX_OUTPUT_RATE);
        node->mSampleRate = static_cast<uint>(srate);
        return;
    }

    WARN("Unhandled sample rate choice type: %u\n", choiceType);
}

void parse_positions(DeviceNode *node, const spa_pod *value)
{
    constexpr size_t MaxChannels{SPA_AUDIO_MAX_CHANNELS};

    auto posdata = std::make_unique<uint32_t[]>(MaxChannels);
    const al::span<uint32_t,MaxChannels> posarray{posdata.get(), MaxChannels};
    if(auto got = get_param_array<SPA_TYPE_Id>(value, posarray))
    {
        const al::span<uint32_t> chanmap{posarray.first(got)};

        /* TODO: Does 5.1(rear) need to be tracked, or will PipeWire do the
         * right thing and re-route the Side-labelled Surround channels to
         * Rear-labelled Surround?
         */
        if(got >= 8 && MatchChannelMap(chanmap, X71Map))
            node->mChannels = DevFmtX71;
        else if(got >= 7 && MatchChannelMap(chanmap, X61Map))
            node->mChannels = DevFmtX61;
        else if(got >= 6 && MatchChannelMap(chanmap, X51Map))
            node->mChannels = DevFmtX51;
        else if(got >= 6 && MatchChannelMap(chanmap, X51RearMap))
            node->mChannels = DevFmtX51;
        else if(got >= 4 && MatchChannelMap(chanmap, QuadMap))
            node->mChannels = DevFmtQuad;
        else if(got >= 2 && MatchChannelMap(chanmap, StereoMap))
            node->mChannels = DevFmtStereo;
        else if(got >= 1)
            node->mChannels = DevFmtMono;
        TRACE("Device ID %u got %u position%s for %s\n", node->mId, got, (got==1)?"":"s",
            DevFmtChannelsString(node->mChannels));
    }
}

void parse_channels(DeviceNode *node, const spa_pod *value)
{
    /* As a fallback with just a channel count, just assume mono or stereo. */
    if(auto chans = get_param<SPA_TYPE_Int>(value))
    {
        if(*chans >= 2)
            node->mChannels = DevFmtStereo;
        else if(*chans >= 1)
            node->mChannels = DevFmtMono;
        TRACE("Device ID %u got %d channel%s for %s\n", node->mId, *chans, (*chans==1)?"":"s",
            DevFmtChannelsString(node->mChannels));
    }
}

void NodeProxy::paramCallback(int, uint32_t id, uint32_t, uint32_t, const spa_pod *param)
{
    if(id == SPA_PARAM_EnumFormat)
    {
        DeviceNode *node{FindDeviceNode(mId)};
        if(unlikely(!node)) return;

        if(const spa_pod_prop *prop{spa_pod_find_prop(param, nullptr, SPA_FORMAT_AUDIO_rate)})
            parse_srate(node, &prop->value);

        if(const spa_pod_prop *prop{spa_pod_find_prop(param, nullptr, SPA_FORMAT_AUDIO_position)})
            parse_positions(node, &prop->value);
        else if((prop=spa_pod_find_prop(param, nullptr, SPA_FORMAT_AUDIO_channels)) != nullptr)
            parse_channels(node, &prop->value);
    }
}


/* A metadata proxy object used to query the default sink and source. */
struct MetadataProxy {
    uint32_t mId{};

    pw_proxy *mProxy{nullptr};
    spa_hook mListener{};

    MetadataProxy(uint32_t id, pw_proxy *proxy)
      : mId{id}, mProxy{proxy}
    {
        pw_proxy_add_object_listener(mProxy, &mListener, &sMetadataEvents, this);
    }
    ~MetadataProxy()
    {
        spa_hook_remove(&mListener);
        pw_proxy_destroy(mProxy);
    }


    int propertyCallback(uint32_t id, const char *key, const char *type, const char *value);
    static int propertyCallbackC(void *object, uint32_t id, const char *key, const char *type,
        const char *value)
    { return static_cast<MetadataProxy*>(object)->propertyCallback(id, key, type, value); }

    static const pw_metadata_events sMetadataEvents;
    static constexpr pw_metadata_events CreateMetadataEvents()
    {
        pw_metadata_events ret{};
        ret.version = PW_VERSION_METADATA_EVENTS;
        ret.property = &MetadataProxy::propertyCallbackC;
        return ret;
    }
};
const pw_metadata_events MetadataProxy::sMetadataEvents{MetadataProxy::CreateMetadataEvents()};

int MetadataProxy::propertyCallback(uint32_t id, const char *key, const char *type,
    const char *value)
{
    if(id != PW_ID_CORE)
        return 0;

    bool isCapture{};
    if(std::strcmp(key, "default.audio.sink") == 0)
        isCapture = false;
    else if(std::strcmp(key, "default.audio.source") == 0)
        isCapture = true;
    else
        return 0;

    if(!type)
    {
        TRACE("Default %s device cleared\n", isCapture ? "capture" : "playback");
        if(!isCapture) DefaultSinkDev.clear();
        else DefaultSourceDev.clear();
        return 0;
    }
    if(std::strcmp(type, "Spa:String:JSON") != 0)
    {
        ERR("Unexpected %s property type: %s\n", key, type);
        return 0;
    }

    spa_json it[2]{};
    spa_json_init(&it[0], value, strlen(value));
    if(spa_json_enter_object(&it[0], &it[1]) <= 0)
        return 0;

    char k[128]{};
    while(spa_json_get_string(&it[1], k, sizeof(k)-1) > 0)
    {
        if(std::strcmp(k, "name") == 0)
        {
            const char *name{};
            int len{spa_json_next(&it[1], &name)};
            if(len <= 0) break;

            std::string nametmp;
            nametmp.resize(static_cast<uint>(len)+1, '\0');
            if(spa_json_parse_string(name, len, &nametmp[0]) <= 0)
                break;
            while(!nametmp.empty() && nametmp.back() == '\0')
                nametmp.pop_back();

            TRACE("Got default %s device \"%s\"\n", isCapture ? "capture" : "playback",
                nametmp.c_str());
            if(!isCapture)
                DefaultSinkDev = nametmp;
            else
                DefaultSourceDev = nametmp;
        }
        else
        {
            const char *v{};
            if(spa_json_next(&it[1], &v) <= 0)
                break;
        }
    }
    return 0;
}


bool EventManager::init()
{
    mLoop = ThreadMainloop{pw_thread_loop_new("PWEventThread", nullptr)};
    if(!mLoop)
    {
        ERR("Failed to create PipeWire event thread loop (errno: %d)\n", errno);
        return false;
    }

    mContext = pw_context_new(mLoop.getLoop(), nullptr, 0);
    if(!mContext)
    {
        ERR("Failed to create PipeWire event context (errno: %d)\n", errno);
        return false;
    }

    mCore = pw_context_connect(mContext, nullptr, 0);
    if(!mCore)
    {
        ERR("Failed to connect PipeWire event context (errno: %d)\n", errno);
        return false;
    }

    mRegistry = pw_core_get_registry(mCore, PW_VERSION_REGISTRY, 0);
    if(!mRegistry)
    {
        ERR("Failed to get PipeWire event registry (errno: %d)\n", errno);
        return false;
    }

    ppw_registry_add_listener(mRegistry, &mRegistryListener, &sRegistryEvents, this);
    ppw_core_add_listener(mCore, &mCoreListener, &sCoreEvents, this);

    /* Set an initial sequence ID for initialization, to trigger after the
     * registry is first populated.
     */
    mInitSeq = ppw_core_sync(mCore, PW_ID_CORE, 0);

    if(int res{mLoop.start()})
    {
        ERR("Failed to start PipeWire event thread loop (res: %d)\n", res);
        return false;
    }

    return true;
}

EventManager::~EventManager()
{
    if(mLoop) mLoop.stop();

    for(NodeProxy *node : mProxyList)
        al::destroy_at(node);
    if(mDefaultMetadata)
        al::destroy_at(mDefaultMetadata);

    if(mRegistry) pw_proxy_destroy(reinterpret_cast<pw_proxy*>(mRegistry));
    if(mCore) pw_core_disconnect(mCore);
    if(mContext) pw_context_destroy(mContext);
}

void EventManager::addCallback(uint32_t id, uint32_t, const char *type, uint32_t version,
    const spa_dict *props)
{
    /* We're only interested in interface nodes. */
    if(std::strcmp(type, PW_TYPE_INTERFACE_Node) == 0)
    {
        const char *media_class{spa_dict_lookup(props, PW_KEY_MEDIA_CLASS)};
        if(!media_class) return;

        /* Specifically, audio sinks and sources. */
        const bool isGood{al::strcasecmp(media_class, AudioSinkClass) == 0
            || al::strcasecmp(media_class, AudioSourceClass) == 0};
        if(!isGood)
            return;

        /* Create the proxy object. */
        auto *proxy = static_cast<pw_proxy*>(pw_registry_bind(mRegistry, id, type, version,
            sizeof(NodeProxy)));
        if(!proxy)
        {
            ERR("Failed to create node proxy object (errno: %d)\n", errno);
            return;
        }

        /* Initialize the NodeProxy to hold the proxy object, add it to the
         * active proxy list, and update the sync point.
         */
        auto *node = static_cast<NodeProxy*>(pw_proxy_get_user_data(proxy));
        mProxyList.emplace_back(al::construct_at(node, id, proxy));
        syncInit();
    }
    else if(std::strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0)
    {
        const char *data_class{spa_dict_lookup(props, PW_KEY_METADATA_NAME)};
        if(!data_class) return;

        if(std::strcmp(data_class, "default") != 0)
        {
            TRACE("Ignoring metadata \"%s\"\n", data_class);
            return;
        }

        if(mDefaultMetadata)
        {
            ERR("Duplicate default metadata\n");
            return;
        }

        auto *proxy = static_cast<pw_proxy*>(pw_registry_bind(mRegistry, id, type, version,
            sizeof(MetadataProxy)));
        if(!proxy)
        {
            ERR("Failed to create metadata proxy object (errno: %d)\n", errno);
            return;
        }

        auto *mdata = static_cast<MetadataProxy*>(pw_proxy_get_user_data(proxy));
        mDefaultMetadata = al::construct_at(mdata, id, proxy);
        syncInit();
    }
}

void EventManager::removeCallback(uint32_t id)
{
    RemoveDevice(id);

    auto elem = mProxyList.begin();
    while(elem != mProxyList.end())
    {
        NodeProxy *node{*elem};
        if(node->mId == id)
        {
            al::destroy_at(node);
            elem = mProxyList.erase(elem);
            continue;
        }
        ++elem;
    }

    if(mDefaultMetadata && mDefaultMetadata->mId == id)
    {
        al::destroy_at(mDefaultMetadata);
        mDefaultMetadata = nullptr;
    }
}

void EventManager::coreCallback(uint32_t id, int seq)
{
    if(id == PW_ID_CORE && seq == mInitSeq)
    {
        /* Initialization done. Remove this callback and signal anyone that may
         * be waiting.
         */
        spa_hook_remove(&mCoreListener);

        mInitDone.store(true);
        mLoop.signal(false);
    }
}


enum use_f32p_e : bool { UseDevType=false, ForceF32Planar=true };
spa_audio_info_raw make_spa_info(DeviceBase *device, use_f32p_e use_f32p)
{
    spa_audio_info_raw info{};
    if(use_f32p)
    {
        device->FmtType = DevFmtFloat;
        info.format = SPA_AUDIO_FORMAT_F32P;
    }
    else switch(device->FmtType)
    {
    case DevFmtByte: info.format = SPA_AUDIO_FORMAT_S8; break;
    case DevFmtUByte: info.format = SPA_AUDIO_FORMAT_U8; break;
    case DevFmtShort: info.format = SPA_AUDIO_FORMAT_S16; break;
    case DevFmtUShort: info.format = SPA_AUDIO_FORMAT_U16; break;
    case DevFmtInt: info.format = SPA_AUDIO_FORMAT_S32; break;
    case DevFmtUInt: info.format = SPA_AUDIO_FORMAT_U32; break;
    case DevFmtFloat: info.format = SPA_AUDIO_FORMAT_F32; break;
    }

    info.rate = device->Frequency;

    al::span<const spa_audio_channel> map{};
    switch(device->FmtChans)
    {
    case DevFmtMono: map = MonoMap; break;
    case DevFmtStereo: map = StereoMap; break;
    case DevFmtQuad: map = QuadMap; break;
    case DevFmtX51: map = X51Map; break;
    case DevFmtX61: map = X61Map; break;
    case DevFmtX71: map = X71Map; break;
    case DevFmtAmbi3D:
        info.flags |= SPA_AUDIO_FLAG_UNPOSITIONED;
        info.channels = device->channelsFromFmt();
        break;
    }
    if(!map.empty())
    {
        info.channels = static_cast<uint32_t>(map.size());
        std::copy(map.begin(), map.end(), info.position);
    }

    return info;
}

class PipeWirePlayback final : public BackendBase {
    void stateChangedCallback(pw_stream_state old, pw_stream_state state, const char *error);
    static void stateChangedCallbackC(void *data, pw_stream_state old, pw_stream_state state,
        const char *error)
    { static_cast<PipeWirePlayback*>(data)->stateChangedCallback(old, state, error); }

    void ioChangedCallback(uint32_t id, void *area, uint32_t size);
    static void ioChangedCallbackC(void *data, uint32_t id, void *area, uint32_t size)
    { static_cast<PipeWirePlayback*>(data)->ioChangedCallback(id, area, size); }

    void outputCallback();
    static void outputCallbackC(void *data)
    { static_cast<PipeWirePlayback*>(data)->outputCallback(); }

    void open(const char *name) override;
    bool reset() override;
    void start() override;
    void stop() override;
    ClockLatency getClockLatency() override;

    uint32_t mTargetId{PwIdAny};
    nanoseconds mTimeBase{0};
    ThreadMainloop mLoop;
    PwStreamPtr mStream;
    spa_io_rate_match *mRateMatch{};
    std::unique_ptr<float*[]> mChannelPtrs;
    uint mNumChannels{};

    static const pw_stream_events sEvents;
    static constexpr pw_stream_events InitEvent()
    {
        pw_stream_events ret{};
        ret.version = PW_VERSION_STREAM_EVENTS;
        ret.state_changed = &PipeWirePlayback::stateChangedCallbackC;
        ret.io_changed = &PipeWirePlayback::ioChangedCallbackC;
        ret.process = &PipeWirePlayback::outputCallbackC;
        return ret;
    }

public:
    PipeWirePlayback(DeviceBase *device) noexcept : BackendBase{device} { }
    ~PipeWirePlayback();

    DEF_NEWDEL(PipeWirePlayback)
};
const pw_stream_events PipeWirePlayback::sEvents{PipeWirePlayback::InitEvent()};

PipeWirePlayback::~PipeWirePlayback()
{
    if(mLoop && mStream)
    {
        /* The main loop needs to be locked when accessing/destroying the
         * stream from user threads.
         */
        MainloopLockGuard _{mLoop};
        mStream = nullptr;
    }
}


void PipeWirePlayback::stateChangedCallback(pw_stream_state, pw_stream_state, const char*)
{ mLoop.signal(false); }

void PipeWirePlayback::ioChangedCallback(uint32_t id, void *area, uint32_t size)
{
    switch(id)
    {
    case SPA_IO_RateMatch:
        if(size >= sizeof(spa_io_rate_match))
            mRateMatch = static_cast<spa_io_rate_match*>(area);
        break;
    }
}

void PipeWirePlayback::outputCallback()
{
    pw_buffer *pw_buf{pw_stream_dequeue_buffer(mStream.get())};
    if(unlikely(!pw_buf)) return;

    /* For planar formats, each datas[] seems to contain one channel, so store
     * the pointers in an array. Limit the render length in case the available
     * buffer length in any one channel is smaller than we wanted (shouldn't
     * be, but just in case).
     */
    spa_data *datas{pw_buf->buffer->datas};
    const size_t chancount{minu(mNumChannels, pw_buf->buffer->n_datas)};
    /* TODO: How many samples should actually be written? 'maxsize' can be 16k
     * samples, which is excessive (~341ms @ 48khz). SPA_IO_RateMatch contains
     * a 'size' field that apparently indicates how many samples should be
     * written per update, but it's not obviously right.
     */
    uint length{mRateMatch ? mRateMatch->size : mDevice->UpdateSize};
    for(size_t i{0};i < chancount;++i)
    {
        length = minu(length, datas[i].maxsize/sizeof(float));
        mChannelPtrs[i] = static_cast<float*>(datas[i].data);
    }

    mDevice->renderSamples({mChannelPtrs.get(), chancount}, length);

    for(size_t i{0};i < chancount;++i)
    {
        datas[i].chunk->offset = 0;
        datas[i].chunk->stride = sizeof(float);
        datas[i].chunk->size   = length * sizeof(float);
    }
    pw_buf->size = length;
    pw_stream_queue_buffer(mStream.get(), pw_buf);
}


void PipeWirePlayback::open(const char *name)
{
    static std::atomic<uint> OpenCount{0};

    uint32_t targetid{PwIdAny};
    std::string devname{};
    if(!name)
    {
        EventWatcherLockGuard _{gEventHandler};
        gEventHandler.waitForInit();

        auto match = DeviceList.cend();
        if(!DefaultSinkDev.empty())
        {
            auto match_default = [](const DeviceNode &n) -> bool
            { return n.mDevName == DefaultSinkDev; };
            match = std::find_if(DeviceList.cbegin(), DeviceList.cend(), match_default);
        }
        if(match == DeviceList.cend())
        {
            auto match_playback = [](const DeviceNode &n) -> bool
            { return !n.mCapture; };
            match = std::find_if(DeviceList.cbegin(), DeviceList.cend(), match_playback);
            if(match == DeviceList.cend())
                throw al::backend_exception{al::backend_error::NoDevice,
                    "No PipeWire playback device found"};
        }

        targetid = match->mId;
        devname = match->mName;
    }
    else
    {
        EventWatcherLockGuard _{gEventHandler};
        gEventHandler.waitForInit();

        auto match_name = [name](const DeviceNode &n) -> bool
        { return !n.mCapture && n.mName == name; };
        auto match = std::find_if(DeviceList.cbegin(), DeviceList.cend(), match_name);
        if(match == DeviceList.cend())
            throw al::backend_exception{al::backend_error::NoDevice,
                "Device name \"%s\" not found", name};

        targetid = match->mId;
        devname = match->mName;
    }

    if(!mLoop)
    {
        const uint count{OpenCount.fetch_add(1, std::memory_order_relaxed)};
        const std::string thread_name{"ALSoftP" + std::to_string(count)};
        mLoop = ThreadMainloop{pw_thread_loop_new(thread_name.c_str(), nullptr)};
        if(!mLoop)
            throw al::backend_exception{al::backend_error::DeviceError,
                "Failed to create PipeWire mainloop (errno: %d)", errno};
        if(int res{mLoop.start()})
            throw al::backend_exception{al::backend_error::DeviceError,
                "Failed to start PipeWire mainloop (res: %d)", res};
    }

    /* TODO: Ensure the target ID is still valid/usable and accepts streams. */

    mTargetId = targetid;
    if(!devname.empty())
        mDevice->DeviceName = std::move(devname);
    else
        mDevice->DeviceName = pwireDevice;
}

bool PipeWirePlayback::reset()
{
    if(mStream)
    {
        MainloopLockGuard _{mLoop};
        mStream = nullptr;
    }
    mRateMatch = nullptr;
    mTimeBase = GetDeviceClockTime(mDevice);

    /* If connecting to a specific device, update various device parameters to
     * match its format.
     */
    mDevice->Flags.reset(DirectEar);
    if(mTargetId != PwIdAny)
    {
        EventWatcherLockGuard _{gEventHandler};

        auto match_id = [targetid=mTargetId](const DeviceNode &n) -> bool
        { return targetid == n.mId; };
        auto match = std::find_if(DeviceList.cbegin(), DeviceList.cend(), match_id);
        if(match != DeviceList.cend())
        {
            if(!mDevice->Flags.test(FrequencyRequest) && match->mSampleRate > 0)
            {
                /* Scale the update size if the sample rate changes. */
                const double scale{static_cast<double>(match->mSampleRate) / mDevice->Frequency};
                mDevice->Frequency = match->mSampleRate;
                mDevice->UpdateSize = static_cast<uint>(clampd(mDevice->UpdateSize*scale + 0.5,
                    64.0, 8192.0));
                mDevice->BufferSize = mDevice->UpdateSize * 2;
            }
            if(!mDevice->Flags.test(ChannelsRequest) && match->mChannels != InvalidChannelConfig)
                mDevice->FmtChans = match->mChannels;
            if(match->mChannels == DevFmtStereo && match->mIsHeadphones)
                mDevice->Flags.set(DirectEar);
        }
    }
    /* Force planar 32-bit float output for playback. This is what PipeWire
     * handles internally, and it's easier for us too.
     */
    spa_audio_info_raw info{make_spa_info(mDevice, ForceF32Planar)};

    /* TODO: How to tell what an appropriate size is? Examples just use this
     * magic value.
     */
    constexpr uint32_t pod_buffer_size{1024};
    auto pod_buffer = std::make_unique<al::byte[]>(pod_buffer_size);
    spa_pod_builder b{make_pod_builder(pod_buffer.get(), pod_buffer_size)};

    const spa_pod *params{spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info)};
    if(!params)
        throw al::backend_exception{al::backend_error::DeviceError,
            "Failed to set PipeWire audio format parameters"};

    pw_properties *props{pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_MEDIA_ROLE, "Game",
        PW_KEY_NODE_ALWAYS_PROCESS, "true",
        nullptr)};
    if(!props)
        throw al::backend_exception{al::backend_error::DeviceError,
            "Failed to create PipeWire stream properties (errno: %d)", errno};

    auto&& binary = GetProcBinary();
    const char *appname{binary.fname.length() ? binary.fname.c_str() : "OpenAL Soft"};
    /* TODO: Which properties are actually needed here? Any others that could
     * be useful?
     */
    pw_properties_set(props, PW_KEY_NODE_NAME, appname);
    pw_properties_set(props, PW_KEY_NODE_DESCRIPTION, appname);
    pw_properties_setf(props, PW_KEY_NODE_LATENCY, "%u/%u", mDevice->UpdateSize,
        mDevice->Frequency);
    pw_properties_setf(props, PW_KEY_NODE_RATE, "1/%u", mDevice->Frequency);

    MainloopUniqueLock plock{mLoop};
    /* The stream takes overship of 'props', even in the case of failure. */
    mStream = PwStreamPtr{pw_stream_new_simple(mLoop.getLoop(), "Playback Stream", props,
        &sEvents, this)};
    if(!mStream)
        throw al::backend_exception{al::backend_error::NoDevice,
            "Failed to create PipeWire stream (errno: %d)", errno};

    constexpr pw_stream_flags Flags{PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_INACTIVE
        | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS};
    if(int res{pw_stream_connect(mStream.get(), PW_DIRECTION_OUTPUT, mTargetId, Flags, &params, 1)})
        throw al::backend_exception{al::backend_error::DeviceError,
            "Error connecting PipeWire stream (res: %d)", res};

    /* Wait for the stream to become paused (ready to start streaming). */
    pw_stream_state state{};
    const char *error{};
    while((state=pw_stream_get_state(mStream.get(), &error)) != PW_STREAM_STATE_PAUSED)
    {
        if(state == PW_STREAM_STATE_ERROR)
            throw al::backend_exception{al::backend_error::DeviceError,
                "Error connecting PipeWire stream: \"%s\"", error};
        mLoop.wait();
    }
    /* TODO: Update mDevice->BufferSize with the total known buffering delay
     * from the head of this playback stream to the tail of the device output.
     */
    mDevice->BufferSize = mDevice->UpdateSize * 2;
    plock.unlock();

    mNumChannels = mDevice->channelsFromFmt();
    mChannelPtrs = std::make_unique<float*[]>(mNumChannels);

    setDefaultWFXChannelOrder();

    return true;
}

void PipeWirePlayback::start()
{
    MainloopLockGuard _{mLoop};
    if(int res{pw_stream_set_active(mStream.get(), true)})
        throw al::backend_exception{al::backend_error::DeviceError,
            "Failed to start PipeWire stream (res: %d)", res};

    /* Wait for the stream to start playing (would be nice to not, but we need
     * the actual update size which is only available after starting).
     */
    pw_stream_state state{};
    const char *error{};
    while((state=pw_stream_get_state(mStream.get(), &error)) == PW_STREAM_STATE_PAUSED)
        mLoop.wait();

    if(state == PW_STREAM_STATE_ERROR)
        throw al::backend_exception{al::backend_error::DeviceError,
            "PipeWire stream error: %s", error ? error : "(unknown)"};
    if(state == PW_STREAM_STATE_STREAMING && mRateMatch && mRateMatch->size)
    {
        mDevice->UpdateSize = mRateMatch->size;
        mDevice->BufferSize = mDevice->UpdateSize * 2;
    }
}

void PipeWirePlayback::stop()
{
    MainloopLockGuard _{mLoop};
    if(int res{pw_stream_set_active(mStream.get(), false)})
        throw al::backend_exception{al::backend_error::DeviceError,
            "Failed to stop PipeWire stream (res: %d)", res};

    /* Wait for the stream to stop playing. */
    while(pw_stream_get_state(mStream.get(), nullptr) == PW_STREAM_STATE_STREAMING)
        mLoop.wait();
}

ClockLatency PipeWirePlayback::getClockLatency()
{
    /* Given a real-time low-latency output, this is rather complicated to get
     * accurate timing. So, here we go.
     */

    /* First, get the stream time info (tick delay, ticks played, and the
     * CLOCK_MONOTONIC time closest to when that last tick was played).
     */
    pw_time ptime{};
    if(mStream)
    {
        MainloopLockGuard _{mLoop};
        if(int res{pw_stream_get_time(mStream.get(), &ptime)})
            ERR("Failed to get PipeWire stream time (res: %d)\n", res);
    }

    /* Now get the mixer time and the CLOCK_MONOTONIC time atomically (i.e. the
     * monotonic clock closest to 'now', and the last mixer time at 'now').
     */
    nanoseconds mixtime{};
    timespec tspec{};
    uint refcount;
    do {
        refcount = mDevice->waitForMix();
        mixtime = GetDeviceClockTime(mDevice);
        clock_gettime(CLOCK_MONOTONIC, &tspec);
        std::atomic_thread_fence(std::memory_order_acquire);
    } while(refcount != ReadRef(mDevice->MixCount));

    /* Convert the monotonic clock, stream ticks, and stream delay to
     * nanoseconds.
     */
    nanoseconds monoclock{seconds{tspec.tv_sec} + nanoseconds{tspec.tv_nsec}};
    nanoseconds curtic{}, delay{};
    if(unlikely(ptime.rate.denom < 1))
    {
        /* If there's no stream rate, the stream hasn't had a chance to get
         * going and return time info yet. Just use dummy values.
         */
        ptime.now = monoclock.count();
        curtic = mixtime;
        delay = nanoseconds{seconds{mDevice->BufferSize}} / mDevice->Frequency;
    }
    else
    {
        /* The stream gets recreated with each reset, so include the time that
         * had already passed with previous streams.
         */
        curtic = mTimeBase;
        /* More safely scale the ticks to avoid overflowing the pre-division
         * temporary as it gets larger.
         */
        curtic += seconds{ptime.ticks / ptime.rate.denom} * ptime.rate.num;
        curtic += nanoseconds{seconds{ptime.ticks%ptime.rate.denom} * ptime.rate.num} /
            ptime.rate.denom;

        /* The delay should be small enough to not worry about overflow. */
        delay = nanoseconds{seconds{ptime.delay} * ptime.rate.num} / ptime.rate.denom;
    }

    /* If the mixer time is ahead of the stream time, there's that much more
     * delay relative to the stream delay.
     */
    if(mixtime > curtic)
        delay += mixtime - curtic;
    /* Reduce the delay according to how much time has passed since the known
     * stream time. This isn't 100% accurate since the system monotonic clock
     * doesn't tick at the exact same rate as the audio device, but it should
     * be good enough with ptime.now being constantly updated every few
     * milliseconds with ptime.ticks.
     */
    delay -= monoclock - nanoseconds{ptime.now};

    /* Return the mixer time and delay. Clamp the delay to no less than 0,
     * incase timer drift got that severe.
     */
    ClockLatency ret{};
    ret.ClockTime = mixtime;
    ret.Latency = std::max(delay, nanoseconds{});

    return ret;
}


class PipeWireCapture final : public BackendBase {
    void stateChangedCallback(pw_stream_state old, pw_stream_state state, const char *error);
    static void stateChangedCallbackC(void *data, pw_stream_state old, pw_stream_state state,
        const char *error)
    { static_cast<PipeWireCapture*>(data)->stateChangedCallback(old, state, error); }

    void inputCallback();
    static void inputCallbackC(void *data)
    { static_cast<PipeWireCapture*>(data)->inputCallback(); }

    void open(const char *name) override;
    void start() override;
    void stop() override;
    void captureSamples(al::byte *buffer, uint samples) override;
    uint availableSamples() override;

    uint32_t mTargetId{PwIdAny};
    ThreadMainloop mLoop;
    PwStreamPtr mStream;

    RingBufferPtr mRing{};

    static const pw_stream_events sEvents;
    static constexpr pw_stream_events InitEvent()
    {
        pw_stream_events ret{};
        ret.version = PW_VERSION_STREAM_EVENTS;
        ret.state_changed = &PipeWireCapture::stateChangedCallbackC;
        ret.process = &PipeWireCapture::inputCallbackC;
        return ret;
    }

public:
    PipeWireCapture(DeviceBase *device) noexcept : BackendBase{device} { }
    ~PipeWireCapture();

    DEF_NEWDEL(PipeWireCapture)
};
const pw_stream_events PipeWireCapture::sEvents{PipeWireCapture::InitEvent()};

PipeWireCapture::~PipeWireCapture()
{
    if(mLoop && mStream)
    {
        MainloopLockGuard _{mLoop};
        mStream = nullptr;
    }
}


void PipeWireCapture::stateChangedCallback(pw_stream_state, pw_stream_state, const char*)
{ mLoop.signal(false); }

void PipeWireCapture::inputCallback()
{
    pw_buffer *pw_buf{pw_stream_dequeue_buffer(mStream.get())};
    if(unlikely(!pw_buf)) return;

    spa_data *bufdata{pw_buf->buffer->datas};
    const uint offset{minu(bufdata->chunk->offset, bufdata->maxsize)};
    const uint size{minu(bufdata->chunk->size, bufdata->maxsize - offset)};

    mRing->write(static_cast<char*>(bufdata->data) + offset, size / mRing->getElemSize());

    pw_stream_queue_buffer(mStream.get(), pw_buf);
}


void PipeWireCapture::open(const char *name)
{
    static std::atomic<uint> OpenCount{0};

    uint32_t targetid{PwIdAny};
    std::string devname{};
    if(!name)
    {
        EventWatcherLockGuard _{gEventHandler};
        gEventHandler.waitForInit();

        auto match = DeviceList.cend();
        if(!DefaultSourceDev.empty())
        {
            auto match_default = [](const DeviceNode &n) -> bool
            { return n.mDevName == DefaultSourceDev; };
            match = std::find_if(DeviceList.cbegin(), DeviceList.cend(), match_default);
        }
        if(match == DeviceList.cend())
        {
            auto match_capture = [](const DeviceNode &n) -> bool
            { return n.mCapture; };
            match = std::find_if(DeviceList.cbegin(), DeviceList.cend(), match_capture);
        }
        if(match == DeviceList.cend())
        {
            auto match_playback = [](const DeviceNode &n) -> bool
            { return !n.mCapture; };
            match = std::find_if(DeviceList.cbegin(), DeviceList.cend(), match_playback);
            if(match == DeviceList.cend())
                throw al::backend_exception{al::backend_error::NoDevice,
                    "No PipeWire capture device found"};
        }

        targetid = match->mId;
        if(match->mCapture) devname = match->mName;
        else devname = MonitorPrefix+match->mName;
    }
    else
    {
        EventWatcherLockGuard _{gEventHandler};
        gEventHandler.waitForInit();

        auto match_name = [name](const DeviceNode &n) -> bool
        { return n.mCapture && n.mName == name; };
        auto match = std::find_if(DeviceList.cbegin(), DeviceList.cend(), match_name);
        if(match == DeviceList.cend() && std::strncmp(name, MonitorPrefix, MonitorPrefixLen) == 0)
        {
            const char *sinkname{name + MonitorPrefixLen};
            auto match_sinkname = [sinkname](const DeviceNode &n) -> bool
            { return !n.mCapture && n.mName == sinkname; };
            match = std::find_if(DeviceList.cbegin(), DeviceList.cend(), match_sinkname);
        }
        if(match == DeviceList.cend())
            throw al::backend_exception{al::backend_error::NoDevice,
                "Device name \"%s\" not found", name};

        targetid = match->mId;
        devname = name;
    }

    if(!mLoop)
    {
        const uint count{OpenCount.fetch_add(1, std::memory_order_relaxed)};
        const std::string thread_name{"ALSoftC" + std::to_string(count)};
        mLoop = ThreadMainloop{pw_thread_loop_new(thread_name.c_str(), nullptr)};
        if(!mLoop)
            throw al::backend_exception{al::backend_error::DeviceError,
                "Failed to create PipeWire mainloop (errno: %d)", errno};
        if(int res{mLoop.start()})
            throw al::backend_exception{al::backend_error::DeviceError,
                "Failed to start PipeWire mainloop (res: %d)", res};
    }

    /* TODO: Ensure the target ID is still valid/usable and accepts streams. */

    mTargetId = targetid;
    if(!devname.empty())
        mDevice->DeviceName = std::move(devname);
    else
        mDevice->DeviceName = pwireInput;


    spa_audio_info_raw info{make_spa_info(mDevice, UseDevType)};

    constexpr uint32_t pod_buffer_size{1024};
    auto pod_buffer = std::make_unique<al::byte[]>(pod_buffer_size);
    spa_pod_builder b{make_pod_builder(pod_buffer.get(), pod_buffer_size)};

    const spa_pod *params[]{spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info)};
    if(!params[0])
        throw al::backend_exception{al::backend_error::DeviceError,
            "Failed to set PipeWire audio format parameters"};

    pw_properties *props{pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Game",
        PW_KEY_NODE_ALWAYS_PROCESS, "true",
        nullptr)};
    if(!props)
        throw al::backend_exception{al::backend_error::DeviceError,
            "Failed to create PipeWire stream properties (errno: %d)", errno};

    auto&& binary = GetProcBinary();
    const char *appname{binary.fname.length() ? binary.fname.c_str() : "OpenAL Soft"};
    pw_properties_set(props, PW_KEY_NODE_NAME, appname);
    pw_properties_set(props, PW_KEY_NODE_DESCRIPTION, appname);
    /* We don't actually care what the latency/update size is, as long as it's
     * reasonable. Unfortunately, when unspecified PipeWire seems to default to
     * around 40ms, which isn't great. So request 20ms instead.
     */
    pw_properties_setf(props, PW_KEY_NODE_LATENCY, "%u/%u", (mDevice->Frequency+25) / 50,
        mDevice->Frequency);

    MainloopUniqueLock plock{mLoop};
    mStream = PwStreamPtr{pw_stream_new_simple(mLoop.getLoop(), "Capture Stream", props,
        &sEvents, this)};
    if(!mStream)
        throw al::backend_exception{al::backend_error::NoDevice,
            "Failed to create PipeWire stream (errno: %d)", errno};

    constexpr pw_stream_flags Flags{PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_INACTIVE
        | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS};
    if(int res{pw_stream_connect(mStream.get(), PW_DIRECTION_INPUT, mTargetId, Flags, params, 1)})
        throw al::backend_exception{al::backend_error::DeviceError,
            "Error connecting PipeWire stream (res: %d)", res};

    /* Wait for the stream to become paused (ready to start streaming). */
    pw_stream_state state{};
    const char *error{};
    while((state=pw_stream_get_state(mStream.get(), &error)) != PW_STREAM_STATE_PAUSED)
    {
        if(state == PW_STREAM_STATE_ERROR)
            throw al::backend_exception{al::backend_error::DeviceError,
                "Error connecting PipeWire stream: \"%s\"", error};
        mLoop.wait();
    }
    plock.unlock();

    setDefaultWFXChannelOrder();

    /* Ensure at least a 100ms capture buffer. */
    mRing = RingBuffer::Create(maxu(mDevice->Frequency/10, mDevice->BufferSize),
        mDevice->frameSizeFromFmt(), false);
}


void PipeWireCapture::start()
{
    MainloopLockGuard _{mLoop};
    if(int res{pw_stream_set_active(mStream.get(), true)})
        throw al::backend_exception{al::backend_error::DeviceError,
            "Failed to start PipeWire stream (res: %d)", res};

    pw_stream_state state{};
    const char *error{};
    while((state=pw_stream_get_state(mStream.get(), &error)) == PW_STREAM_STATE_PAUSED)
        mLoop.wait();

    if(state == PW_STREAM_STATE_ERROR)
        throw al::backend_exception{al::backend_error::DeviceError,
            "PipeWire stream error: %s", error ? error : "(unknown)"};
}

void PipeWireCapture::stop()
{
    MainloopLockGuard _{mLoop};
    if(int res{pw_stream_set_active(mStream.get(), false)})
        throw al::backend_exception{al::backend_error::DeviceError,
            "Failed to stop PipeWire stream (res: %d)", res};

    while(pw_stream_get_state(mStream.get(), nullptr) == PW_STREAM_STATE_STREAMING)
        mLoop.wait();
}

uint PipeWireCapture::availableSamples()
{ return static_cast<uint>(mRing->readSpace()); }

void PipeWireCapture::captureSamples(al::byte *buffer, uint samples)
{ mRing->read(buffer, samples); }

} // namespace


bool PipeWireBackendFactory::init()
{
    if(!pwire_load())
        return false;

    pw_init(0, nullptr);

    /* TODO: Check that audio devices are supported. */

    return gEventHandler.init();
}

bool PipeWireBackendFactory::querySupport(BackendType type)
{ return type == BackendType::Playback || type == BackendType::Capture; }

std::string PipeWireBackendFactory::probe(BackendType type)
{
    std::string outnames;

    EventWatcherLockGuard _{gEventHandler};
    gEventHandler.waitForInit();

    auto match_defsink = [](const DeviceNode &n) -> bool
    { return n.mDevName == DefaultSinkDev; };
    auto match_defsource = [](const DeviceNode &n) -> bool
    { return n.mDevName == DefaultSourceDev; };

    auto sort_devnode = [](DeviceNode &lhs, DeviceNode &rhs) noexcept -> bool
    { return lhs.mId < rhs.mId; };
    std::sort(DeviceList.begin(), DeviceList.end(), sort_devnode);

    auto defmatch = DeviceList.cbegin();
    switch(type)
    {
    case BackendType::Playback:
        defmatch = std::find_if(defmatch, DeviceList.cend(), match_defsink);
        if(defmatch != DeviceList.cend())
        {
            /* Includes null char. */
            outnames.append(defmatch->mName.c_str(), defmatch->mName.length()+1);
        }
        for(auto iter = DeviceList.cbegin();iter != DeviceList.cend();++iter)
        {
            if(iter != defmatch && !iter->mCapture)
                outnames.append(iter->mName.c_str(), iter->mName.length()+1);
        }
        break;
    case BackendType::Capture:
        defmatch = std::find_if(defmatch, DeviceList.cend(), match_defsource);
        if(defmatch != DeviceList.cend())
        {
            if(!defmatch->mCapture)
                outnames.append(MonitorPrefix);
            outnames.append(defmatch->mName.c_str(), defmatch->mName.length()+1);
        }
        for(auto iter = DeviceList.cbegin();iter != DeviceList.cend();++iter)
        {
            if(iter != defmatch && iter->mCapture)
                outnames.append(iter->mName.c_str(), iter->mName.length()+1);
        }
        for(auto iter = DeviceList.cbegin();iter != DeviceList.cend();++iter)
        {
            if(iter != defmatch && !iter->mCapture)
                outnames.append(MonitorPrefix).append(iter->mName.c_str(), iter->mName.length()+1);
        }
        break;
    }

    return outnames;
}

BackendPtr PipeWireBackendFactory::createBackend(DeviceBase *device, BackendType type)
{
    if(type == BackendType::Playback)
        return BackendPtr{new PipeWirePlayback{device}};
    if(type == BackendType::Capture)
        return BackendPtr{new PipeWireCapture{device}};
    return nullptr;
}

BackendFactory &PipeWireBackendFactory::getFactory()
{
    static PipeWireBackendFactory factory{};
    return factory;
}
