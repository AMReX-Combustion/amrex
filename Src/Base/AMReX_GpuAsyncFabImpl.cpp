#include <AMReX_GpuAsyncFabImpl.H>

#ifdef AMREX_USE_GPU

#include <AMReX_GpuDevice.H>
#include <AMReX_Vector.H>
#include <cstring>
#include <mutex>

#ifdef _OPENMP
#include <omp.h>
#endif

namespace {
    void destroy (amrex::FArrayBox* p) {
        if (p) {
            p->~FArrayBox();
            amrex::The_Managed_Arena()->free(p);
        }
    }
}

namespace amrex {
namespace Gpu {

//
// Fab is Managed
//

namespace {
    bool initialized = false;

    std::mutex asyncfab_mutex;

    Vector<Vector<std::unique_ptr<FArrayBox, void(*)(FArrayBox*)> > > fab_stacks;

    inline Vector<std::unique_ptr<FArrayBox, void(*)(FArrayBox*)> >& get_stack ()
    {
#ifdef _OPENMP
        int tid = omp_get_thread_num();
#else
        int tid = 0;
#endif
        return fab_stacks[tid];
    }
}

void
AsyncFabImpl::Initialize ()
{
    if (initialized) return;
    initialized = true;

#ifdef _OPENMP
    int nthreads = omp_get_max_threads();
#else
    int nthreads = 1;
#endif
    fab_stacks.resize(nthreads);
}

void
AsyncFabImpl::Finalize ()
{
    fab_stacks.clear();
}


AsyncFabImpl::AsyncFabImpl ()
    : m_gpu_fab(nullptr, destroy)
{
    static_assert(AMREX_IS_TRIVIALLY_COPYABLE(BaseFabData<Real>),
                  "BaseFabData must be trivially copyable");
    auto& fabstack = get_stack();
    bool do_copy = false;
    {
        std::lock_guard<std::mutex> guard(asyncfab_mutex);
        if (fabstack.empty()) {
            FArrayBox* p = static_cast<FArrayBox*>(The_Managed_Arena()->alloc(sizeof(FArrayBox)));
            new (p) FArrayBox;
            m_gpu_fab.reset(p);
        } else {
            m_gpu_fab = std::move(fabstack.back());
            fabstack.pop_back();
            do_copy = true;
        }
    }
    if (do_copy) copy_htod();
}

AsyncFabImpl::AsyncFabImpl (Box const& bx, int ncomp)
    : m_cpu_fab(bx,ncomp),
      m_gpu_fab(nullptr, destroy)
{
    auto& fabstack = get_stack();
    {
        std::lock_guard<std::mutex> guard(asyncfab_mutex);
        if (fabstack.empty()) {
            FArrayBox* p = static_cast<FArrayBox*>(The_Managed_Arena()->alloc(sizeof(FArrayBox)));
            new (p) FArrayBox;
            m_gpu_fab.reset(p);  // yes, we build an empty fab here, later it will be overwritten by copy_htod
        } else {
            m_gpu_fab = std::move(fabstack.back());
            fabstack.pop_back();
        }
    }
    copy_htod();
}

AsyncFabImpl::AsyncFabImpl (FArrayBox& a_fab)
    : m_gpu_fab(nullptr, destroy)
{
    if (a_fab.isAllocated()) {
        m_cpu_fab.resize(a_fab.box(), a_fab.nComp());
    }
    auto& fabstack = get_stack();
    {
        std::lock_guard<std::mutex> guard(asyncfab_mutex);
        if (fabstack.empty()) {
            FArrayBox* p = static_cast<FArrayBox*>(The_Managed_Arena()->alloc(sizeof(FArrayBox)));
            new (p) FArrayBox;
            m_gpu_fab.reset(p);  // yes, we build an empty fab here, later it will be overwritten by copy_htod
        } else {
            m_gpu_fab = std::move(fabstack.back());
            fabstack.pop_back();
        }
    }
    copy_htod();
}

AsyncFabImpl::AsyncFabImpl (FArrayBox& /*a_fab*/, Box const& bx, int ncomp)
    : AsyncFabImpl(bx,ncomp)
{}

AsyncFabImpl::~AsyncFabImpl ()
{
    std::lock_guard<std::mutex> guard(asyncfab_mutex);
    auto& fabstack = get_stack();
    fabstack.push_back(std::move(m_gpu_fab));
}

FArrayBox*
AsyncFabImpl::fabPtr () noexcept
{
    AMREX_ASSERT(m_gpu_fab != nullptr);
    return m_gpu_fab.get();
}

FArrayBox&
AsyncFabImpl::hostFab () noexcept
{
    return m_cpu_fab;
}

void
AsyncFabImpl::copy_htod ()
{
    auto dest = static_cast<BaseFabData<Real>*>(m_gpu_fab.get());
    if (Gpu::inLaunchRegion())
    {
        m_cpu_fab_data = m_cpu_fab;
        m_cpu_fab_data.setOwner(false);
        Gpu::htod_memcpy_async(dest, &m_cpu_fab_data, sizeof(BaseFabData<Real>));
    }
    else
    {
        auto src  = static_cast<BaseFabData<Real>*>(&m_cpu_fab);
        std::memcpy(dest, src, sizeof(BaseFabData<Real>));
        dest->setOwner(false);
    }
}

}
}

#else

namespace amrex {
namespace Gpu {

void AsyncFabImpl::Initialize () {}
void AsyncFabImpl::Finalize () {}

AsyncFabImpl::AsyncFabImpl () {}

AsyncFabImpl::AsyncFabImpl (Box const& bx, int ncomp)
    : m_cpu_fab(bx,ncomp), m_cpu_fab_alias(&m_cpu_fab) {}

AsyncFabImpl::AsyncFabImpl (FArrayBox& a_fab) : m_cpu_fab_alias (&a_fab) {}

AsyncFabImpl::AsyncFabImpl (FArrayBox& a_fab, Box const& bx, int ncomp)
    : m_cpu_fab_alias(&a_fab)
{
    m_cpu_fab_alias->resize(bx,ncomp);
}

AsyncFabImpl::~AsyncFabImpl () {}

FArrayBox* AsyncFabImpl::fabPtr () noexcept { return m_cpu_fab_alias; }

FArrayBox& AsyncFabImpl::hostFab () noexcept { return *m_cpu_fab_alias; }

}}

#endif