#if defined(PS2X_G777_NATIVE_GL)
struct G778ShadowJob {std::shared_ptr<dc2_g778::Command> command;bool begin;};
static void g778FlushExec()
{
    if(g260HasPending())g260ExecuteAll(kG260NonDef,true);
    if(!g_g144List.empty() && g_g144FlushFn)g_g144FlushFn();
}
static void g778ExecShadow(void* value)
{
    std::unique_ptr<G778ShadowJob> job(static_cast<G778ShadowJob*>(value));
    auto& command=job->command;
    if(command->nativeCommitted.load(std::memory_order_acquire)){
        if(job->begin || command->avoidedStarts!=1 || command->avoidedResumes!=12){
            ++dc2_g778::counters.transportFailures;std::abort();
        }
        command->finished=true;
        if(dc2_g778::statsRequested())std::fprintf(stderr,
            "[G778:ordered-native] draw=%llu sf=%u state=%x geometry=%x call=%x node=%x avoided=%u/%u clip=%08x unpack=%u\n",
            (unsigned long long)command->draw->invocation,command->draw->frame,command->association.state,
            command->association.geometry,command->association.call,command->association.listNode,
            command->avoidedStarts.load(),command->avoidedResumes.load(),command->clip,command->transportBytes);
        return;
    }
    if(job->begin){
        g778FlushExec();
        if(dc2_g778::shadowCommand){++dc2_g778::counters.associationRejected;return;}
        dc2_g778::shadowCommand=command;++dc2_g778::counters.orderedBegins;
    }else{
        if(dc2_g778::shadowCommand.get()!=command.get()){
            ++dc2_g778::counters.associationRejected;++command->shadowFailures;
        }else{
            g778FlushExec();
            if(command->coreStarts!=1 || command->coreResumes!=12){
                ++command->shadowFailures;++dc2_g778::counters.associationRejected;
            }
            dc2_g778::shadowCommand.reset();++dc2_g778::counters.orderedEnds;
        }
        command->finished=true;
        if(dc2_g778::statsRequested())std::fprintf(stderr,
            "[G778:ordered] draw=%llu sf=%u state=%x geometry=%x call=%x node=%x cores=%u/%u batches=%u failures=%u\n",
            (unsigned long long)command->draw->invocation,command->draw->frame,command->association.state,
            command->association.geometry,command->association.call,command->association.listNode,
            command->coreStarts.load(),command->coreResumes.load(),command->shadowBatches.load(),command->shadowFailures.load());
    }
}
#endif
