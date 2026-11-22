// Private DX11 implementation, included by dx11DetailManager_VS.cpp only.
// Preparation runs on the immediate context before any detail draw is recorded.
struct CDetailManager::WindCache
{
    struct Entry
    {
        SlotItem* owner{}; // Identity only: never dereference an evicted owner.
        u32 seen{};
        Fvector4 rows[3]{};
    };
    struct Job
    {
        Fvector4 rows[3];
        u32 slot, vertexCount, padding[2];
    };
    static_assert(sizeof(Job) == 64, "HLSL job layout");
    struct Model
    {
        ID3D11Buffer* source{};
        ID3D11ShaderResourceView* sourceView{};
        ID3D11Buffer* output{};
        ID3D11ShaderResourceView* outputView{};
        ID3D11UnorderedAccessView* outputUAV{};
        ID3D11Buffer* jobs{};
        ID3D11ShaderResourceView* jobsView{};
        u32 capacity{};
        xr_vector<Entry> entries; // Indexed by the same stable slot as the GPU output.
        xr_vector<u32> freeSlots;
        xr_vector<Job> pending;
        const Entry* Find(const SlotItem* item, u32 object) const
        {
            const u32 slot = item->wind_slot;
            return item->wind_model == object && slot < entries.size() && entries[slot].owner == item
                ? &entries[slot] : nullptr;
        }
        Entry* Find(const SlotItem* item, u32 object)
        {
            return const_cast<Entry*>(static_cast<const Model&>(*this).Find(item, object));
        }
        void Evict(u32 frame)
        {
            for (u32 slot = 0; slot < entries.size(); ++slot)
            {
                Entry& entry = entries[slot];
                if (entry.owner && entry.seen != frame)
                {
                    entry.owner = nullptr;
                    freeSlots.push_back(slot);
                }
            }
        }
        Entry& Acquire(SlotItem* item, u32 object, u32 frame)
        {
            VERIFY(!freeSlots.empty());
            const u32 slot = freeSlots.back();
            freeSlots.pop_back();
            Entry& entry = entries[slot];
            entry = Entry{};
            entry.owner = item;
            entry.seen = frame;
            item->wind_slot = slot;
            item->wind_model = object;
            return entry;
        }
        ~Model()
        {
            _RELEASE(sourceView); _RELEASE(source);
            _RELEASE(outputUAV); _RELEASE(outputView); _RELEASE(output);
            _RELEASE(jobsView); _RELEASE(jobs);
        }
    };
    xr_vector<Model*> models;
    ref_cs shader;
    double animationStep = 1.0 / 30.0;
    double lastTime = -1.0;
    u64 tick = u64(-1);
    FrameMode mode = FrameMode::Still;
    u32 frame = u32(-1);
    u32 reportTime{};
    double reportGameTime{};
    u32 frames{}, updates{}, windUpdates{}, dispatches{}, invalidations{};
    bool diagnostics{};
    std::atomic<u32> renderCalls{0}, shadowCalls{0};
    struct Sample
    {
        ID3D11Query* stats{};
        ID3D11Query* disjoint{};
        ID3D11Query* begin{};
        ID3D11Query* end{};
        bool pending{};
    };
    Sample samples[8];
    u32 sampleIndex{};
    u64 completedCS{};
    double gpuMilliseconds{};
    u32 completedSamples{};

    WindCache(const DetailVec& objects)
    {
        diagnostics = strstr(Core.Params, "-detail_wind_stats") != nullptr;
        reportTime = Device.dwTimeContinual;
        reportGameTime = double(Device.dwTimeGlobal) / 1000.0;
        if (pSettings->line_exist("details", "swing_animation_hz"))
        {
            float hz = pSettings->r_float("details", "swing_animation_hz");
            clamp(hz, 15.f, 120.f);
            animationStep = 1.0 / hz;
        }
        shader = RImplementation.Resources->_CreateCS("detail_wind_update");
        for (const CDetail* object : objects)
        {
            Model* model = xr_new<Model>();
            models.push_back(model);
            xr_vector<Fvector4> vertices(object->number_vertices);
            const float height = object->bv_bb.vMax.y - object->bv_bb.vMin.y;
            for (u32 v = 0; v < object->number_vertices; ++v)
            {
                const Fvector& p = object->vertices[v].P;
                int fraction = iFloor(p.y / height * 16384.f);
                clamp(fraction, -32768, 32767);
                vertices[v].set(p.x, p.y, p.z, float(fraction) / 16384.f);
            }
            CreateBuffer(model->source, model->sourceView, nullptr,
                object->number_vertices, sizeof(Fvector4), vertices.data(), false);
        }
        if (diagnostics)
            for (Sample& sample : samples)
            {
                D3D11_QUERY_DESC desc{D3D11_QUERY_PIPELINE_STATISTICS, 0};
                CHK_DX(HW.pDevice->CreateQuery(&desc, &sample.stats));
                desc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
                CHK_DX(HW.pDevice->CreateQuery(&desc, &sample.disjoint));
                desc.Query = D3D11_QUERY_TIMESTAMP;
                CHK_DX(HW.pDevice->CreateQuery(&desc, &sample.begin));
                CHK_DX(HW.pDevice->CreateQuery(&desc, &sample.end));
            }
        if (diagnostics)
            Msg("* [detail wind] GPU cache, step=%.9f s; VS=detail_c (no wind)", animationStep);
    }
    ~WindCache()
    {
        for (Model*& model : models)
            xr_delete(model);
        for (Sample& sample : samples)
        {
            _RELEASE(sample.stats); _RELEASE(sample.disjoint);
            _RELEASE(sample.begin); _RELEASE(sample.end);
        }
    }
    static void CreateBuffer(ID3D11Buffer*& buffer, ID3D11ShaderResourceView*& view,
        ID3D11UnorderedAccessView** uav, u32 count, u32 stride, const void* data, bool dynamic)
    {
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = count * stride;
        desc.Usage = dynamic ? D3D11_USAGE_DYNAMIC : data ? D3D11_USAGE_IMMUTABLE : D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | (uav ? D3D11_BIND_UNORDERED_ACCESS : 0);
        desc.CPUAccessFlags = dynamic ? D3D11_CPU_ACCESS_WRITE : 0;
        // Dynamic CPU-write resources cannot carry STRUCTURED misc flags.
        // Jobs use a typed float4 SRV; immutable source/output are structured.
        desc.MiscFlags = dynamic ? 0 : D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        desc.StructureByteStride = dynamic ? 0 : stride;
        D3D11_SUBRESOURCE_DATA initial{};
        initial.pSysMem = data;
        CHK_DX(HW.pDevice->CreateBuffer(&desc, data ? &initial : nullptr, &buffer));
        D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        srv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        srv.Buffer.NumElements = count * stride / sizeof(Fvector4);
        CHK_DX(HW.pDevice->CreateShaderResourceView(buffer, dynamic ? &srv : nullptr, &view));
        if (uav)
            CHK_DX(HW.pDevice->CreateUnorderedAccessView(buffer, nullptr, uav));
    }
    Sample* BeginSample(ID3D11DeviceContext* context)
    {
        if (!diagnostics)
            return nullptr;
        for (Sample& s : samples)
        {
            if (!s.pending)
                continue;
            D3D11_QUERY_DATA_PIPELINE_STATISTICS stats{};
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
            UINT64 begin{}, end{};
            constexpr UINT flags = D3D11_ASYNC_GETDATA_DONOTFLUSH;
            if (context->GetData(s.end, &end, sizeof(end), flags) == S_OK &&
                context->GetData(s.begin, &begin, sizeof(begin), flags) == S_OK &&
                context->GetData(s.stats, &stats, sizeof(stats), flags) == S_OK &&
                context->GetData(s.disjoint, &disjoint, sizeof(disjoint), flags) == S_OK)
            {
                completedCS += stats.CSInvocations;
                if (!disjoint.Disjoint)
                    gpuMilliseconds += double(end - begin) * 1000.0 / double(disjoint.Frequency);
                ++completedSamples;
                s.pending = false;
            }
        }
        Sample& s = samples[sampleIndex++ % 8];
        if (s.pending)
            return nullptr; // GPU is behind: skip measurement, never wait.
        context->Begin(s.stats);
        context->Begin(s.disjoint);
        context->End(s.begin);
        return &s;
    }
    void EndSample(ID3D11DeviceContext* context, Sample* sample)
    {
        if (!sample)
            return;
        context->End(sample->end);
        context->End(sample->disjoint);
        context->End(sample->stats);
        sample->pending = true;
    }
};

void CDetailManager::hw_UnloadWind()
{
    xr_delete(m_windCache);
}

void CDetailManager::hw_PrepareWind()
{
    if (!m_windCache || !dtFS || !psDeviceFlags.is(rsDrawDetails) ||
        m_windCache->frame == Device.dwFrame)
        return;
    ZoneScoped;
    auto& cache = *m_windCache;
    auto& cmd = RCache;
    VERIFY(cmd.context_id == CHW::IMM_CTX_ID);
    // The same visibility dependency already used by Render; no GPU wait.
    if (m_calc_task)
        TaskScheduler->Wait(*m_calc_task);
    cache.frame = Device.dwFrame;
    if (cache.diagnostics)
        ++cache.frames;
    m_modeFrameNumber = Device.dwFrame;
    m_windFrameNumber = Device.dwFrame;
    m_frameMode = m_requestedFrameMode;

    const double now = double(Device.dwTimeGlobal) / 1000.0;
    const double delta = cache.lastTime < 0.0 || now < cache.lastTime ? 0.0 : now - cache.lastTime;
    const bool rewind = cache.lastTime >= 0.0 && now < cache.lastTime;
    cache.lastTime = now;
    m_global_time_old = Device.fTimeGlobal;
    const u64 tick = static_cast<u64>(floor(now / cache.animationStep));
    const bool advance = rewind || tick != cache.tick || cache.mode != m_frameMode;
    cache.tick = tick;
    cache.mode = m_frameMode;

    if (m_frameMode != FrameMode::Still)
    {
        swing_current.lerp(swing_desc[0], swing_desc[1],
            g_pGamePersistent->Environment().wind_strength_factor);
        // Integrate elapsed game time once, including stalls; never replay missed dispatches.
        m_time_pos += float(delta) * swing_current.speed;
        float& rotation = m_frameMode == FrameMode::Wave1 ? m_time_rot_1 : m_time_rot_2;
        rotation += PI_MUL_2 * float(delta) /
            (m_frameMode == FrameMode::Wave1 ? swing_current.rot1 : swing_current.rot2);
        if (advance)
        {
            m_frameWind.set(_sin(rotation), 0.f, _cos(rotation), 0.f).normalize().mul(
                m_frameMode == FrameMode::Wave1 ? swing_current.amp1 : swing_current.amp2);
            if (m_frameMode == FrameMode::Wave1)
                m_frameWave.set(1.f / 5.f, 1.f / 7.f, 1.f / 3.f, m_time_pos);
            else
                m_frameWave.set(1.f / 3.f, 1.f / 7.f, 1.f / 5.f, m_time_pos);
            m_frameWave.div(PI_MUL_2);
        }
    }

    auto* context = HW.get_context(CHW::IMM_CTX_ID);
    auto* sample = cache.BeginSample(context);
    bool dispatched = false;
    for (u32 o = 0; o < objects.size(); ++o)
    {
        auto& model = *cache.models[o];
        model.pending.clear();
        if (m_frameMode == FrameMode::Still)
        {
            // Retain allocations, but discard identities so re-entry always initializes.
            if (model.entries.empty())
                continue;
            model.entries.clear();
            model.freeSlots.clear();
            continue;
        }
        u32 count = 0;
        for (u32 group = 0; group < 3; ++group)
            for (const VisibleItems& visible : m_visibles[group][o])
                count += static_cast<u32>(visible.items->size());
        if (!count)
        {
            // No instance may use the previous geometry now. Invalidate all
            // handles at once; keep GPU allocations for the next visible frame.
            model.entries.clear();
            model.freeSlots.clear();
            continue;
        }
        // Restore the retained range before growth, including a Still -> Wave
        // transition that also needs a larger GPU buffer.
        if (model.entries.empty() && model.capacity)
        {
            model.entries.resize(model.capacity);
            for (u32 i = model.capacity; i != 0; --i)
                model.freeSlots.push_back(i - 1);
        }
        bool resized = false;
        if (count > model.capacity)
        {
            const u32 oldCapacity = model.capacity;
            model.capacity = oldCapacity ? oldCapacity * 2 : 256;
            while (model.capacity < count)
                model.capacity *= 2;
            _RELEASE(model.outputUAV); _RELEASE(model.outputView); _RELEASE(model.output);
            _RELEASE(model.jobsView); _RELEASE(model.jobs);
            WindCache::CreateBuffer(model.output, model.outputView, &model.outputUAV,
                model.capacity * objects[o]->number_vertices, sizeof(Fvector4), nullptr, false);
            WindCache::CreateBuffer(model.jobs, model.jobsView, nullptr,
                model.capacity, sizeof(WindCache::Job), nullptr, true);
            model.entries.resize(model.capacity);
            for (u32 i = model.capacity; i != oldCapacity; --i)
                model.freeSlots.push_back(i - 1);
            resized = true;
        }
        // Mark before evicting: surviving instances retain their GPU addresses.
        // A stale handle can never resurrect an evicted/reassigned slot.
        for (u32 group = 0; group < 3; ++group)
            for (const VisibleItems& visible : m_visibles[group][o])
                for (SlotItem* item : *visible.items)
                    if (auto* entry = model.Find(item, o))
                        entry->seen = Device.dwFrame;
        model.Evict(Device.dwFrame);

        for (u32 group = 0; group < 3; ++group)
            for (const VisibleItems& visible : m_visibles[group][o])
                for (SlotItem* item : *visible.items)
                {
                    auto* cachedEntry = model.Find(item, o);
                    const bool fresh = !cachedEntry;
                    auto& entry = fresh ? model.Acquire(item, o, Device.dwFrame) : *cachedEntry;
                    const Fmatrix& m = item->mRotY;
                    const float s = item->scale_calculated;
                    WindCache::Job job{};
                    job.rows[0].set(m._11*s, m._21*s, m._31*s, m._41);
                    job.rows[1].set(m._12*s, m._22*s, m._32*s, m._42);
                    job.rows[2].set(m._13*s, m._23*s, m._33*s, m._43);
                    const bool changed = fresh || memcmp(entry.rows, job.rows, sizeof(entry.rows)) != 0;
                    entry.seen = Device.dwFrame;
                    if (advance || resized || changed)
                    {
                        CopyMemory(entry.rows, job.rows, sizeof(entry.rows));
                        job.slot = item->wind_slot;
                        job.vertexCount = objects[o]->number_vertices;
                        model.pending.push_back(job);
                        if (cache.diagnostics && (changed || resized))
                            ++cache.invalidations;
                    }
                }
        if (model.pending.empty())
            continue;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        CHK_DX(context->Map(model.jobs, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
        CopyMemory(mapped.pData, model.pending.data(), model.pending.size() * sizeof(WindCache::Job));
        context->Unmap(model.jobs, 0);
        PIX_EVENT_CTX(cmd, DETAIL_WIND_UPDATE);
        cmd.set_CS(cache.shader);
        cmd.set_Constants(&cache.shader->constants);
        cmd.set_c("detail_wind_wave", m_frameWave);
        cmd.set_c("detail_wind_dir", m_frameWind);
        cmd.SRVSManager.SetCSResource(0, model.sourceView);
        cmd.SRVSManager.SetCSResource(1, model.jobsView);
        context->CSSetUnorderedAccessViews(0, 1, &model.outputUAV, nullptr);
        // One group per instance, with a loop over that model's vertices.
        // D3D11 limits each dimension to 65535 groups.
        for (u32 start = 0; start < model.pending.size(); start += 65535)
        {
            cmd.set_c("detail_job_start", float(start), 0.f, 0.f, 0.f);
            cmd.Compute(_min(u32(model.pending.size()) - start, 65535u), 1, 1);
            if (cache.diagnostics)
                ++cache.dispatches; // Counts actual Dispatch calls, not constant updates.
        }
        ID3D11UnorderedAccessView* nullUAV = nullptr;
        context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
        dispatched = true;
    }
    if (dispatched)
    {
        if (cache.diagnostics)
        {
            ++cache.updates;
            if (advance)
                ++cache.windUpdates;
        }
        cmd.SRVSManager.SetCSResource(0, nullptr);
        cmd.SRVSManager.SetCSResource(1, nullptr);
        cmd.SRVSManager.Apply(CHW::IMM_CTX_ID);
        cmd.set_CS(nullptr);
    }
    cache.EndSample(context, sample);
    if (cache.diagnostics && Device.dwTimeContinual - cache.reportTime >= 1000)
    {
        const double seconds = double(Device.dwTimeContinual - cache.reportTime) / 1000.0;
        size_t bytes = 0;
        for (u32 o = 0; o < objects.size(); ++o)
            bytes += size_t(cache.models[o]->capacity) *
                (objects[o]->number_vertices * sizeof(Fvector4) + sizeof(WindCache::Job)) +
                objects[o]->number_vertices * sizeof(Fvector4);
        Msg("* [detail wind] wall_s=%.3f game_s=%.3f frames=%u render_calls=%u shadow_calls=%u update_frames=%u wind_ticks=%u dispatch=%u invalidated_instances=%u "
            "GPU_CS=%llu GPU_samples=%u GPU_update_ms_total=%.4f buffers=%zu bytes step=%.9f",
            seconds, now - cache.reportGameTime, cache.frames, cache.renderCalls.exchange(0, std::memory_order_relaxed),
            cache.shadowCalls.exchange(0, std::memory_order_relaxed),
            cache.updates, cache.windUpdates, cache.dispatches, cache.invalidations,
            cache.completedCS, cache.completedSamples, cache.gpuMilliseconds, bytes, cache.animationStep);
        cache.reportTime = Device.dwTimeContinual;
        cache.reportGameTime = now;
        cache.frames = cache.updates = cache.windUpdates = cache.dispatches = cache.invalidations = 0;
        cache.completedCS = 0; cache.completedSamples = 0; cache.gpuMilliseconds = 0;
    }
}
