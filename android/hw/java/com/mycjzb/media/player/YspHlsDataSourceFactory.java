package com.mycjzb.media.player;

import androidx.media3.common.C;
import androidx.media3.datasource.DataSource;
import androidx.media3.exoplayer.hls.HlsDataSourceFactory;

import com.mycjzb.media.player.n.a;

/**
 * HLS：仅媒体分片走 YspTeaDataSource；清单/密钥只剥 ysp-tea query。
 */
final class YspHlsDataSourceFactory implements HlsDataSourceFactory {
    private final DataSource.Factory base;
    private final int mode;

    YspHlsDataSourceFactory(DataSource.Factory base, int mode) {
        this.base = base;
        this.mode = mode;
    }

    @Override
    public DataSource createDataSource(@C.DataType int dataType) {
        if (mode == a.m1 || mode == a.m2) {
            if (dataType == C.DATA_TYPE_MEDIA) {
                return new YspTeaDataSource(base.createDataSource(), mode);
            }
            /* 清单/密钥：剥 query，不解 TEA */
            return new YspTeaDataSource(base.createDataSource(), 0);
        }
        return base.createDataSource();
    }
}
