module.exports = {
  apps: [
    {
      name: "cloud-catalog-sync",
      script: "catalog_sync_service.py",
      interpreter: "python3",
      cwd: __dirname,
      instances: 1,
      autorestart: true,
      watch: false,
      max_memory_restart: "512M",
      error_file: "logs/catalog-err.log",
      out_file: "logs/catalog-out.log",
      merge_logs: true,
      time: true,
      env: {
        CS_CATALOG_LOCALE: "en-US",
        CS_CATALOG_REGION: "PL",
        CS_CATALOG_SYNC_INTERVAL_SEC: "3600",
      },
    },
  ],
};
