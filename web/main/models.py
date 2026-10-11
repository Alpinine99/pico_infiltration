from django.db import models
from datetime import datetime, timezone

# Create your models here.

class Reading(models.Model):
    id = models.AutoField(primary_key=True)
    ts = models.IntegerField()                          # unix seconds (UTC), set by the C server
    device = models.TextField()
    uptime_s = models.BigIntegerField(null=True)        # uint32 on the Pico, so too big for IntegerField
    heap_free = models.BigIntegerField(null=True)
    heap_min_free = models.BigIntegerField(null=True)
    battery_mv = models.IntegerField(null=True)
    d0 = models.FloatField(null=True)                   # NULL = no echo / out of range
    d1 = models.FloatField(null=True)
    d2 = models.FloatField(null=True)

    class Meta:
        managed = False            # the C server owns the table; Django must never create or alter it
        db_table = "readings"
        ordering = ["-id"]

    @property
    def time(self):
        return datetime.fromtimestamp(self.ts, tz=timezone.utc)

    @property
    def distances(self):
        return [self.d0, self.d1, self.d2]

