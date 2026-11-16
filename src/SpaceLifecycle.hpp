#ifndef AMDHV_SPACE_LIFECYCLE_HPP
#define AMDHV_SPACE_LIFECYCLE_HPP
#include <src/AppleHVABI.hpp>
#include <src/EPTSource.hpp>
namespace SpaceLifecycle {
using Result=GuestMemory::Result;
// Lives as long as the registered NPT callbacks. Framework callers retain their
// AppleHV object for the whole hook. An update can nest only on that same thread.
class Context {
    struct Space {
        NPTManager::SpaceKey key;
        uintptr_t map {},pmap {},thread {};
        uint32_t depth {};
        bool bound {},retired {};
        NPTManager::Update update;
    } spaces_[4];
    GuestMemory::Registry memory_;
    EPTSource::Adapter source_;
    Runtime::Gate gate_;
    Runtime::AtomicBool failed_ {false};
    bool configured_ {};
    void *threadContext_ {};
    uintptr_t (*thread_)(void *) {};
    Space *find(uint64_t owner);
    void reap();
    void end(size_t index,uintptr_t thread);
public:
    class Mutation {
        Context *context_ {};size_t index_ {};uintptr_t thread_ {};
        Mutation(Context *context,size_t index,uintptr_t thread):context_(context),index_(index),thread_(thread) {}
        friend class Context;
    public:
        Mutation()=default;~Mutation() {reset();}
        Mutation(const Mutation &)=delete;Mutation &operator=(const Mutation &)=delete;
        Mutation(Mutation &&other) noexcept:context_(other.context_),index_(other.index_),thread_(other.thread_) {other.context_=nullptr;}
        void reset() {if(context_) context_->end(index_,thread_);context_=nullptr;}
        explicit operator bool() const {return context_!=nullptr;}
        Result unwire(uint64_t gpa,uint64_t size);
        Result pinFaultPage(uint64_t gpa);
        Result retire();
    };
    bool configure(GuestMemory::Operations memory,EPTSource::Operations source,NPTManager::Shootdown shootdown,
                   uintptr_t (*thread)(void *),void *threadContext=nullptr);
    Result attach(const AppleHVABI::Space &snapshot);
    Mutation begin(const AppleHVABI::Space &snapshot);
    NPTManager::SpaceKey lookup(const AppleHVABI::Space &snapshot);
    // Authoritative absence check for cleanup of partially initialized objects.
    // Contention is not interpreted as absence.
    bool untracked(uint64_t owner);
    bool empty();
    bool ready();
    bool healthy() const {return !failed_;}
};
Context &global();
}
#endif
