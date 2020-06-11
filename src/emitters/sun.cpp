#include <mitsuba/render/emitter.h>
#include <mitsuba/core/properties.h>
#include <mitsuba/core/bitmap.h>
#include <mitsuba/core/warp.h>
#include "sunsky/sunmodel.h"

NAMESPACE_BEGIN(mitsuba)

/* Apparent radius of the sun as seen from the earth (in degrees).
   This is an approximation--the actual value is somewhere between
   0.526 and 0.545 depending on the time of year */
#define SUN_APP_RADIUS 0.5358f

template <typename Float, typename Spectrum>
class SunEmitter final : public Emitter<Float, Spectrum> {
public:
    MTS_IMPORT_BASE(Emitter, m_flags, m_world_transform)
    MTS_IMPORT_TYPES(Scene, Texture)

    SunEmitter(const Properties &props) : Base(props) {
        m_resolution = props.int_("resolution", 512);
        m_scale = props.float_("scale", 1.0f);
        m_sun_radius_scale = props.float_("sun_radius_scale", 1.0f);
        m_sun = compute_sun_coordinates<Float>(props);
        m_turbidity = props.float_("turbidity", 3.0f);
        m_stretch = props.float_("stretch", 1.0f);
        m_flags = EmitterFlags::Infinite | EmitterFlags::SpatiallyVarying;

        SphericalCoordinates<Float> sun(m_sun);
        sun.elevation *= m_stretch;
        m_sun_dir = to_sphere(sun);

        /* Solid angle covered by the sun */
        m_theta = deg_to_rad(SUN_APP_RADIUS * 0.5f);
        m_solid_angle = 2 * math::Pi<Float> * (1 - cos(m_theta));

        Float factor = m_scale;
        if (m_sun_radius_scale == 0) {
            factor *= m_solid_angle;
        }
        m_radiance = compute_sun_radiance<Float, Spectrum>(m_sun.elevation, m_turbidity, factor);
    }

    /// Van der Corput radical inverse in base 2 with double precision
    inline double radicalInverse2Double(uint64_t n, uint64_t scramble = 0ULL) const {
        /* Efficiently reverse the bits in 'n' using binary operations */
    #if (defined(__GNUC__) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 2))) || defined(__clang__)
        n = __builtin_bswap64(n);
    #else
        printf("!!!!!!!!!!!!!!!!!!!!! radical else\n");
        n = (n << 32) | (n >> 32);
        n = ((n & 0x0000ffff0000ffffULL) << 16) | ((n & 0xffff0000ffff0000ULL) >> 16);
        n = ((n & 0x00ff00ff00ff00ffULL) << 8)  | ((n & 0xff00ff00ff00ff00ULL) >> 8);
    #endif
        n = ((n & 0x0f0f0f0f0f0f0f0fULL) << 4)  | ((n & 0xf0f0f0f0f0f0f0f0ULL) >> 4);
        n = ((n & 0x3333333333333333ULL) << 2)  | ((n & 0xccccccccccccccccULL) >> 2);
        n = ((n & 0x5555555555555555ULL) << 1)  | ((n & 0xaaaaaaaaaaaaaaaaULL) >> 1);

        // Account for the available precision and scramble
        n = (n >> (64 - 53)) ^ (scramble & ~-(1LL << 53));

        return (double) n / (double) (1ULL << 53);
    }

    /// Sobol' radical inverse in base 2 with double precision.
    inline double sobol2Double(uint64_t n, uint64_t scramble = 0ULL) const {
        scramble &= ~-(1LL << 53);
        for (uint64_t v = 1ULL << 52; n != 0; n >>= 1, v ^= v >> 1)
            if (n & 1)
                scramble ^= v;
        return (double) scramble / (double) (1ULL << 53);
    }

    /// Generate an element from a (0, 2) sequence (without scrambling)
    inline Point2f sample02(size_t n) const {
        return Point2f(
            radicalInverse2Double((uint64_t) n),
            sobol2Double((uint64_t) n)
        );
    }

    std::vector<ref<Object>> expand() const override {
        if (m_sun_radius_scale == 0) {
            Properties props("directional");
            const Transform4f &transf = m_world_transform->eval(0);
            props.set_vector3f("direction", -(transf * m_sun_dir));
            const ref<Object> radiance = (Object*) m_radiance.get();
            props.set_object("irradiance", radiance);

            ref<Object> emitter = PluginManager::instance()->create_object<Base>(props).get();
            
            return {emitter};
        }

        // compute a *very* rough estimate of how many pixel in 
        // the output environment map will be covered by the sun
        size_t pixel_count = m_resolution * m_resolution * 0.5f;
        Float cos_theta = cos(m_theta * m_sun_radius_scale);

        // Ratio of the sphere that is covered by the sun
        Float covered_portion = 0.5f * (1 - cos_theta);

        // Approximate the number of samples that need to be generated
        size_t n_samples = (size_t) max(100.f, pixel_count * covered_portion * 1000.f);

        ref<Bitmap> bitmap = new Bitmap(Bitmap::PixelFormat::RGB, Struct::Type::Float32, 
            Vector2i(m_resolution, m_resolution / 2));
        bitmap->clear();
        Frame<Float> frame(m_sun_dir);

        Point2f factor(bitmap->width() / (2 * math::Pi<Float>), 
            bitmap->height() / math::Pi<Float>);

        SurfaceInteraction3f si;
        si.wavelengths = 0/0;   // TODO: change when implementing spectral mode

        ScalarFloat *target = (ScalarFloat *) bitmap->data();
        Spectrum value = m_radiance->eval(si) * (2 * math::Pi<Float> * 
            (1 - cos(m_theta))) * (Float) (bitmap->width() * bitmap->height()) /
            (2 * math::Pi<Float> * math::Pi<Float> * n_samples);

        for (size_t i = 0; i < n_samples; ++i) {
            Vector3f dir = frame.to_world(warp::square_to_uniform_cone<Float>(sample02(i), cos_theta));

            Float sin_theta = safe_sqrt(1 - dir.y() * dir.y());
            SphericalCoordinates sph_coords = from_sphere(dir);

            Point2i pos(
                min(max(0, (int) (sph_coords.azimuth * factor.x())), bitmap->width() - 1),
                min(max(0, (int) (sph_coords.elevation * factor.y())), bitmap->height() - 1)
            );

            size_t index = (pos.x() + pos.y() * bitmap->width()) * bitmap->channel_count();
            Spectrum val = value / max((Float) 1e-3f, sin_theta);

            target[index] += val.x();
            target[index+1] += val.y();
            target[index+2] += val.z();
        }
        
        Properties prop("envmap");
        prop.set_pointer("bitmap", bitmap.get());
        ref<Object> texture = PluginManager::instance()->create_object<Base>(prop).get();
        
        return {texture};
    }

    // This emitter does not occupy any particular region of space, return an invalid bounding box
    ScalarBoundingBox3f bbox() const override {
        return ScalarBoundingBox3f();
    }

    std::string to_string() const override {
        std::ostringstream oss;
        oss << "SunEmitter[" << std::endl
            << "  sun_dir = " << m_sun_dir << ","  << std::endl
            << "  sun_radius_scale = " << m_sun_radius_scale << "," << std::endl
            << "  turbidity = " << m_turbidity << "," << std::endl
            << "  resolution = " << m_resolution << ","  << std::endl
            << "  stretch = " << m_stretch << ","  << std::endl
            << "  scale = " << m_scale << std::endl
            << "]";
        return oss.str();
    }

    MTS_DECLARE_CLASS()
protected:
    /// Environment map resolution in pixels
    int m_resolution;
    /// Constant scale factor applied to the model
    Float m_scale;
    /// Scale factor that can be applied to the sun radius
    Float m_sun_radius_scale;
    /// Angle cutoff for the sun disk (w/o scaling)
    Float m_theta;
    /// Solid angle covered by the sun (w/o scaling)
    Float m_solid_angle;
    /// Position of the sun in spherical coordinates
    SphericalCoordinates<Float> m_sun;
    /// Direction of the sun (untransformed)
    Vector<Float, 3> m_sun_dir;
    /// Radiance arriving from the sun disk
    ref<Texture> m_radiance;
    /// Atmosphere turbidity
    Float m_turbidity;
    /// Stretch factor to extend to the bottom hemisphere
    Float m_stretch;
};

MTS_IMPLEMENT_CLASS_VARIANT(SunEmitter, Emitter)
MTS_EXPORT_PLUGIN(SunEmitter, "Sun emitter")
NAMESPACE_END(mitsuba)